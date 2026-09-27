// signals.cpp — guest signals: queueing, delivery at syscall boundaries
// (x86-64 rt_sigframe on the guest stack, like arch/x86/kernel/signal.c),
// rt_sigreturn, default actions. Asynchronous delivery into a thread that is
// executing JIT code is not done yet: such a thread sees the signal at its
// next syscall (FEX's frontend does the JIT-context reconstruction; C3+).
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>

#include <cstring>

#include "log.h"
#include "process.h"

namespace rlk {

namespace {

using FEXCore::X86State::REG_R10;
using FEXCore::X86State::REG_R11;
using FEXCore::X86State::REG_R12;
using FEXCore::X86State::REG_R13;
using FEXCore::X86State::REG_R14;
using FEXCore::X86State::REG_R15;
using FEXCore::X86State::REG_R8;
using FEXCore::X86State::REG_R9;
using FEXCore::X86State::REG_RAX;
using FEXCore::X86State::REG_RBP;
using FEXCore::X86State::REG_RBX;
using FEXCore::X86State::REG_RCX;
using FEXCore::X86State::REG_RDI;
using FEXCore::X86State::REG_RDX;
using FEXCore::X86State::REG_RSI;
using FEXCore::X86State::REG_RSP;

constexpr uint64_t kUnblockable = lx::sigbit(lx::sigkill) | lx::sigbit(lx::sigstop);
constexpr uint64_t kDF = 0x400;

// Default action "ignore" (job-control stops are ignored too: no terminal).
bool default_ignored(int sig) {
    return sig == lx::sigchld || sig == 23 /*URG*/ || sig == 28 /*WINCH*/ || sig == lx::sigcont || sig == lx::sigstop ||
           sig == lx::sigtstp || sig == 21 /*TTIN*/ || sig == 22 /*TTOU*/;
}

void fill_default_info(lx::siginfo& si, int sig) {
    memset(&si, 0, sizeof si);
    si.si_signo = sig;
    si.si_code = lx::si_user;
    si.u.kill.pid = t_current_proc ? t_current_proc->pid : 0;
    si.u.kill.uid = 1000;
}

// fxsave layout (512 bytes): fcw@0 fsw@2 ftw@4 fop@6 rip@8 rdp@16 mxcsr@24
// mxcsr_mask@28 st0-7@32 (16 bytes each) xmm0-15@160 (16 bytes each).
void save_fpstate(const FEXCore::Core::CPUState& S, uint8_t* out) {
    memset(out, 0, lx::fxsave_size);
    memcpy(out + 0, &S.FCW, 2);
    out[4] = S.AbridgedFTW;
    memcpy(out + 24, &S.mxcsr, 4);
    const uint32_t mask = 0xFFFF;
    memcpy(out + 28, &mask, 4);
    for (int i = 0; i < 8; i++) memcpy(out + 32 + 16 * i, S.mm[i], 16);
    for (int i = 0; i < 16; i++) memcpy(out + 160 + 16 * i, S.xmm.avx.data[i], 16);
}

void load_fpstate(FEXCore::Core::CPUState& S, const uint8_t* in) {
    memcpy(&S.FCW, in + 0, 2);
    S.AbridgedFTW = in[4];
    memcpy(&S.mxcsr, in + 24, 4);
    for (int i = 0; i < 8; i++) memcpy(S.mm[i], in + 32 + 16 * i, 16);
    for (int i = 0; i < 16; i++) memcpy(S.xmm.avx.data[i], in + 160 + 16 * i, 16);
}

}  // namespace

bool Kernel::has_deliverable(GuestThread& t) const {
    const uint64_t pend = t.sigpending | t.proc->sigpending;
    return (pend & ~t.sigmask) != 0;
}

int Kernel::send_signal(GuestProcess& p, int sig, const lx::siginfo* info) {
    if (sig == 0) return 0;
    if (sig < 1 || sig >= lx::nsig) return -lx::einval;
    {
        std::lock_guard<std::mutex> lk(sig_mu);
        const uint64_t h = p.sigactions[sig].handler;
        if (sig != lx::sigkill && (h == lx::sig_ign || (h == lx::sig_dfl && default_ignored(sig)))) return 0;
        p.sigpending |= lx::sigbit(sig);
        if (info) {
            p.siginfo_by_sig[sig] = *info;
        } else {
            fill_default_info(p.siginfo_by_sig[sig], sig);
        }
    }
    std::lock_guard<std::mutex> lk(mu);  // the thread list may grow (clone) meanwhile
    for (auto& th : p.threads) {
        if (!th->exited && !(th->sigmask & lx::sigbit(sig))) th->waiter.notify();
    }
    return 0;
}

int Kernel::send_signal_thread(GuestThread& t, int sig, const lx::siginfo* info) {
    if (sig == 0) return 0;
    if (sig < 1 || sig >= lx::nsig) return -lx::einval;
    {
        std::lock_guard<std::mutex> lk(sig_mu);
        const uint64_t h = t.proc->sigactions[sig].handler;
        if (sig != lx::sigkill && (h == lx::sig_ign || (h == lx::sig_dfl && default_ignored(sig)))) return 0;
        t.sigpending |= lx::sigbit(sig);
        if (info) {
            t.siginfo_by_sig[sig] = *info;
        } else {
            fill_default_info(t.siginfo_by_sig[sig], sig);
        }
    }
    t.waiter.notify();
    return 0;
}

int64_t Kernel::wait_for_signal(GuestThread& t) {
    for (;;) {
        const uint64_t gen = t.waiter.prepare();
        if (has_deliverable(t) || t.proc->state.load() != (int)ProcState::Running) break;
        t.waiter.wait(gen, 0);
    }
    return -lx::eintr;
}

void Kernel::deliver_signals(GuestThread& t, void* frame_, int64_t ret) {
    auto* frame = static_cast<FEXCore::Core::CpuStateFrame*>(frame_);
    auto& S = frame->State;
    GuestProcess& p = *t.proc;
    auto* thread = static_cast<FEXCore::Core::InternalThreadState*>(t.fex_thread);
    bool interrupted = (ret == -lx::eintr) && t.restartable;
    auto restart = [&] {
        // Re-execute the syscall after the handler: back to the `syscall`
        // instruction with the original number in RAX.
        S.rip -= 2;
        S.gregs[REG_RAX] = t.syscall_nr;
        interrupted = false;
    };
    for (int round = 0; round < 8; round++) {
        int sig = 0;
        lx::siginfo info {};
        {
            std::lock_guard<std::mutex> lk(sig_mu);
            const uint64_t avail = (t.sigpending | p.sigpending) & ~t.sigmask;
            if (!avail) break;
            sig = __builtin_ctzll(avail) + 1;
            const uint64_t bit = lx::sigbit(sig);
            if (t.sigpending & bit) {
                t.sigpending &= ~bit;
                info = t.siginfo_by_sig[sig];
            } else {
                p.sigpending &= ~bit;
                info = p.siginfo_by_sig[sig];
            }
        }
        const lx::sigaction act = p.sigactions[sig];
        if (act.handler == lx::sig_ign || (act.handler == lx::sig_dfl && default_ignored(sig))) {
            if (interrupted) restart();  // the wakeup was for nothing: transparent restart
            continue;
        }
        if (act.handler == lx::sig_dfl) {
            Log("[%d] killed by signal %d (default action)", p.pid, sig);
            exit_thread(t, frame, 128 + sig, true, sig);
            return;
        }
        if (interrupted && (act.flags & lx::sa_restart)) restart();

        // ---- build the frame on the guest stack ----
        const uint64_t bit = lx::sigbit(sig);
        uint64_t sp = S.gregs[REG_RSP];
        const bool alt_ok = (act.flags & lx::sa_onstack) && !(t.altstack_flags & lx::ss_disable) && t.altstack_size;
        const bool on_alt_now = alt_ok && sp > t.altstack_sp && sp <= t.altstack_sp + t.altstack_size;
        if (alt_ok && !on_alt_now) {
            sp = t.altstack_sp + t.altstack_size;
        } else {
            sp -= 128;  // red zone
        }
        const uint64_t fp = (sp - lx::fxsave_size) & ~63ull;
        const uint64_t frame_addr = ((fp - sizeof(lx::rt_sigframe)) & ~15ull) - 8;
        if (!p.mm || !p.mm->is_mapped(frame_addr, fp + lx::fxsave_size - frame_addr)) {
            Log("[%d] signal %d: no room for the frame at 0x%llx; killed", p.pid, sig, (unsigned long long)frame_addr);
            exit_thread(t, frame, 128 + lx::sigsegv, true, lx::sigsegv);
            return;
        }
        const uint32_t eflags = fex_context()->ReconstructCompactedEFLAGS(thread, false, nullptr, 0);
        lx::rt_sigframe fr {};
        fr.pretcode = act.restorer;
        fr.uc.uc_stack.ss_sp = t.altstack_sp;
        fr.uc.uc_stack.ss_size = t.altstack_size;
        fr.uc.uc_stack.ss_flags = (t.altstack_flags & lx::ss_disable) ? lx::ss_disable : (on_alt_now ? lx::ss_onstack : 0);
        auto& sc = fr.uc.uc_mcontext;
        sc.r8 = S.gregs[REG_R8];
        sc.r9 = S.gregs[REG_R9];
        sc.r10 = S.gregs[REG_R10];
        sc.r11 = S.gregs[REG_R11];
        sc.r12 = S.gregs[REG_R12];
        sc.r13 = S.gregs[REG_R13];
        sc.r14 = S.gregs[REG_R14];
        sc.r15 = S.gregs[REG_R15];
        sc.rdi = S.gregs[REG_RDI];
        sc.rsi = S.gregs[REG_RSI];
        sc.rbp = S.gregs[REG_RBP];
        sc.rbx = S.gregs[REG_RBX];
        sc.rdx = S.gregs[REG_RDX];
        sc.rax = S.gregs[REG_RAX];
        sc.rcx = S.gregs[REG_RCX];
        sc.rsp = S.gregs[REG_RSP];
        sc.rip = S.rip;
        sc.eflags = eflags;
        sc.cs = 0x33;
        sc.ss = 0x2b;
        // rt_sigsuspend: the frame carries the mask from before the call, so
        // sigreturn puts it back once the handler returns.
        const uint64_t saved_mask = t.saved_mask_valid ? t.saved_mask : t.sigmask;
        t.saved_mask_valid = false;
        sc.oldmask = saved_mask;
        sc.cr2 = (sig == lx::sigsegv || sig == lx::sigbus) ? info.u.fault.addr : 0;
        sc.fpstate = fp;
        fr.uc.uc_sigmask = saved_mask;
        fr.info = info;
        fr.info.si_signo = sig;
        uint8_t fx[lx::fxsave_size];
        save_fpstate(S, fx);
        if (!p.mm->copy_in(frame_addr, &fr, sizeof fr) || !p.mm->copy_in(fp, fx, sizeof fx)) {
            Log("[%d] signal %d: cannot write the frame; killed", p.pid, sig);
            exit_thread(t, frame, 128 + lx::sigsegv, true, lx::sigsegv);
            return;
        }
        // ---- enter the handler ----
        S.gregs[REG_RSP] = frame_addr;
        S.rip = act.handler;
        S.gregs[REG_RDI] = (uint64_t)sig;
        S.gregs[REG_RSI] = frame_addr + offsetof(lx::rt_sigframe, info);
        S.gregs[REG_RDX] = frame_addr + offsetof(lx::rt_sigframe, uc);
        S.gregs[REG_RAX] = 0;
        fex_context()->SetFlagsFromCompactedEFLAGS(thread, eflags & ~kDF);
        if (act.flags & lx::sa_resethand) p.sigactions[sig].handler = lx::sig_dfl;
        t.sigmask = (t.sigmask | act.mask | ((act.flags & lx::sa_nodefer) ? 0 : bit)) & ~kUnblockable;
        Log("[%d] --- signal %d delivered: handler=0x%llx frame=0x%llx rip=0x%llx", p.pid, sig, (unsigned long long)act.handler,
            (unsigned long long)frame_addr, (unsigned long long)sc.rip);
        return;  // one handler per boundary; more arrive at the next one
    }
}

int64_t Kernel::sigreturn(GuestThread& t, void* frame_) {
    auto* frame = static_cast<FEXCore::Core::CpuStateFrame*>(frame_);
    auto& S = frame->State;
    GuestProcess& p = *t.proc;
    auto* thread = static_cast<FEXCore::Core::InternalThreadState*>(t.fex_thread);
    // The handler's `ret` popped pretcode: rsp points at uc.
    const uint64_t frame_addr = S.gregs[REG_RSP] - 8;
    lx::rt_sigframe fr {};
    if (!p.mm || !p.mm->is_mapped(frame_addr, sizeof fr)) {
        Log("[%d] rt_sigreturn: bad frame 0x%llx; killed", p.pid, (unsigned long long)frame_addr);
        exit_thread(t, frame, 128 + lx::sigsegv, true, lx::sigsegv);
        return 0;
    }
    memcpy(&fr, reinterpret_cast<const void*>(frame_addr), sizeof fr);
    const auto& sc = fr.uc.uc_mcontext;
    S.gregs[REG_R8] = sc.r8;
    S.gregs[REG_R9] = sc.r9;
    S.gregs[REG_R10] = sc.r10;
    S.gregs[REG_R11] = sc.r11;
    S.gregs[REG_R12] = sc.r12;
    S.gregs[REG_R13] = sc.r13;
    S.gregs[REG_R14] = sc.r14;
    S.gregs[REG_R15] = sc.r15;
    S.gregs[REG_RDI] = sc.rdi;
    S.gregs[REG_RSI] = sc.rsi;
    S.gregs[REG_RBP] = sc.rbp;
    S.gregs[REG_RBX] = sc.rbx;
    S.gregs[REG_RDX] = sc.rdx;
    S.gregs[REG_RAX] = sc.rax;
    S.gregs[REG_RCX] = sc.rcx;
    S.gregs[REG_RSP] = sc.rsp;
    S.rip = sc.rip;
    fex_context()->SetFlagsFromCompactedEFLAGS(thread, (uint32_t)sc.eflags);
    if (sc.fpstate && p.mm->is_mapped(sc.fpstate, lx::fxsave_size)) {
        load_fpstate(S, reinterpret_cast<const uint8_t*>(sc.fpstate));
    }
    t.sigmask = fr.uc.uc_sigmask & ~kUnblockable;
    t.no_retval = true;
    return 0;
}

}  // namespace rlk
