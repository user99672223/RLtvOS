// process.cpp — process/thread lifecycle and the FEXCore glue: spawn from
// the app, clone (threads, fork, vfork), execve, wait4, exit.
// FEXCore and host headers come first: linux_abi.h (via process.h) is
// macro-clean but FEX's headers use the host macros inline.
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/SignalScopeGuards.h>

#include "rlfex_internal.h"

#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "futex.h"
#include "log.h"
#include "process.h"

namespace rlk {

// ------------------------------------------------------------------ helpers

thread_local GuestProcess* t_current_proc = nullptr;
thread_local GuestThread* t_current_thread = nullptr;

void GuestProcess::append_output(int fd, std::string_view data) {
    std::lock_guard<std::mutex> lk(out_mu);
    output.append(data);
    if (output.size() > 65536) output.erase(0, output.size() - 65536);
    // one log line per newline-terminated chunk (keeps the console readable)
    size_t start = 0;
    while (start < data.size()) {
        size_t nl = data.find('\n', start);
        std::string_view line = data.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        Log("guest[%d] %s: %.*s", pid, fd == 2 ? "stderr" : "stdout", (int)std::min<size_t>(line.size(), 400), line.data());
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
}

size_t GuestProcess::live_threads() const {
    size_t n = 0;
    for (auto& t : threads) n += t->exited ? 0 : 1;
    return n;
}

Kernel& Kernel::get() {
    static Kernel* k = new Kernel();
    return *k;
}

void Kernel::log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

void Kernel::strace(GuestThread& t, const char* name, const char* args, int64_t ret) {
    if (ret < 0 && ret > -4096) {
        Log("[%d] %s(%s) = -1 %s", t.proc->pid, name, args, ErrnoName((int)-ret));
    } else if (ret > 0xFFFF) {
        Log("[%d] %s(%s) = 0x%llx", t.proc->pid, name, args, (unsigned long long)ret);
    } else {
        Log("[%d] %s(%s) = %lld", t.proc->pid, name, args, (long long)ret);
    }
}

GuestProcess* Kernel::find(int pid) {
    auto it = procs.find(pid);
    return it == procs.end() ? nullptr : it->second.get();
}

// ------------------------------------------------------------------ FEX

namespace {

FEXCore::Context::Context* g_ctx = nullptr;

// Live FEX thread objects (for cache invalidation across threads).
std::mutex g_fex_threads_mu;
std::vector<FEXCore::Core::InternalThreadState*> g_fex_threads;

// Drop every translation of [start, start+len): FEX's lookup cache is shared
// between threads and keyed by guest address, so a page that is unmapped,
// remapped or rewritten would otherwise keep executing its old code (seen
// on the TV in build-15: every fex self-test ran the first test's block).
void invalidate_range(uint64_t start, uint64_t len) {
    if (!g_ctx || len == 0) return;
    auto* self = t_current_thread ? static_cast<FEXCore::Core::InternalThreadState*>(t_current_thread->fex_thread) : nullptr;
    auto lk = FEXCore::GuardSignalDeferringSectionWithFallback(g_ctx->GetCodeInvalidationMutex(), self);
    g_ctx->InvalidateCodeBuffersCodeRange(start, len);
    std::lock_guard<std::mutex> tl(g_fex_threads_mu);
    for (auto* th : g_fex_threads) g_ctx->InvalidateThreadCachedCodeRange(th, start, len);
}

class KernelSyscallHandler final : public FEXCore::HLE::SyscallHandler, public FEXCore::Allocator::FEXAllocOperators {
public:
    void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
        auto* t = static_cast<GuestThread*>(Frame->Thread->FrontendPtr);
        Kernel::get().handle_syscall(*t, Frame);
    }
    void InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState*, uint64_t Start, uint64_t Length) override {
        invalidate_range(Start, Length);
    }
    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
        return {0, UINT64_MAX, true};
    }
    std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*,
                                                                                  uint64_t) override {
        return std::nullopt;
    }
};

class KernelSignalDelegator final : public FEXCore::SignalDelegator, public FEXCore::Allocator::FEXAllocOperators {};

KernelSyscallHandler* g_syscalls = nullptr;
KernelSignalDelegator* g_signals = nullptr;
std::array<FEXCore::Core::CPUState::gdt_segment, 32> g_gdt {};

void init_segments(FEXCore::Core::CPUState& st) {
    using CS = FEXCore::Core::CPUState;
    st.segment_arrays[CS::SEGMENT_ARRAY_INDEX_GDT] = g_gdt.data();
    st.segment_arrays[CS::SEGMENT_ARRAY_INDEX_LDT] = g_gdt.data();
    st.cs_idx = CS::DEFAULT_USER_CS << 3;
    auto* cs = CS::GetSegmentFromIndex(st, st.cs_idx);
    CS::SetGDTBase(cs, 0);
    CS::SetGDTLimit(cs, 0xF'FFFFU);
    cs->L = 1;
    cs->D = 0;
    st.cs_cached = CS::CalculateGDTBase(*cs);
}

// Host signal number → Linux signal number for a guest fault report.
int linux_signal_of_host(int sig) {
    switch (sig) {
        case SIGSEGV: return lx::sigsegv;
        case SIGBUS: return lx::sigbus;
        case SIGILL: return lx::sigill;
        case SIGTRAP: return lx::sigtrap;
        case SIGFPE: return lx::sigfpe;
        default: return lx::sigsegv;
    }
}

bool waiter_interrupted(void* arg) {
    auto* t = static_cast<GuestThread*>(arg);
    if (t->in_vfork_wait) return false;
    return Kernel::get().has_deliverable(*t) || t->proc->state.load() != (int)ProcState::Running;
}

}  // namespace

FEXCore::Context::Context* Kernel::fex_context() {
    return g_ctx;
}

bool Kernel::host_fault_hook(int sig, int code, uint64_t addr, uint64_t pc) {
    (void)sig;
    (void)code;
    (void)pc;
    GuestProcess* p = t_current_proc;
    if (!p || !p->mm) return false;
    return p->mm->cow_fault(addr);
}

bool Kernel::ensure_fex(std::string& err) {
    std::lock_guard<std::mutex> lk(mu);
    if (fex_ready) return true;
    char e[256] = "";
    if (!rlfex_platform_init(e, sizeof e)) {
        err = e;
        return false;
    }
    if (!g_ctx) {
        auto features = rlfex_host_features();
        auto ctx = FEXCore::Context::Context::CreateNewContext(features);
        if (!ctx) {
            err = "CreateNewContext failed";
            return false;
        }
        g_ctx = ctx.release();
        g_syscalls = new KernelSyscallHandler();
        g_signals = new KernelSignalDelegator();
        g_ctx->SetSignalDelegator(g_signals);
        g_ctx->SetSyscallHandler(g_syscalls);
        g_ctx->EnableExitOnHLT();
        if (!g_ctx->InitCore()) {
            err = "InitCore failed";
            return false;
        }
        // AT_HWCAP = CPUID.1:EDX, AT_HWCAP2 bit 1 = FSGSBASE (CPUID.7:EBX bit 0)
        auto r1 = g_ctx->RunCPUIDFunction(1, 0);
        auto r7 = g_ctx->RunCPUIDFunction(7, 0);
        hwcap = r1.edx;
        hwcap2 = (r7.ebx & 1) ? 2 : 0;
        void* hp = ::mmap(nullptr, AddressSpace::host_page(), host::kProtRead | host::kProtWrite,
                          host::kMapPrivate | host::kMapAnon, -1, 0);
        if (hp == host::MapFailed()) {
            err = "hlt page";
            return false;
        }
        static_cast<uint8_t*>(hp)[0] = 0xF4;  // hlt: exit_group parks threads here
        hlt_page = reinterpret_cast<uint64_t>(hp);
        AddressSpace::set_invalidate_hook(&invalidate_range);
        rlfex_set_fault_hook(&Kernel::host_fault_hook);
    }
    fex_ready = true;
    Log("kernel: FEX context ready (hwcap=0x%llx hwcap2=0x%llx)", (unsigned long long)hwcap, (unsigned long long)hwcap2);
    return true;
}

bool Kernel::create_fex_thread(GuestThread& t, std::string& err, const void* initial_state) {
    FEXCore::Core::CPUState st {};
    if (initial_state) {
        memcpy(&st, initial_state, sizeof st);  // CPUState is not copy-assignable; FEX memcpy's it too
    } else {
        st.rip = t.proc->layout.entry;
        st.gregs[FEXCore::X86State::REG_RSP] = t.proc->layout.rsp;
    }
    init_segments(st);
    auto* thread = g_ctx->CreateThread(&st);
    if (!thread) {
        err = "CreateThread failed";
        return false;
    }
    thread->FrontendPtr = &t;
    // call-ret stack: 4 MB RW with a guard page on each side
    const size_t cr = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
    const size_t hp = AddressSpace::host_page();
    void* alloc = ::mmap(nullptr, cr + 2 * hp, host::kProtNone, host::kMapPrivate | host::kMapAnon, -1, 0);
    if (alloc == host::MapFailed()) {
        err = "callret stack";
        g_ctx->DestroyThread(thread);
        return false;
    }
    uint8_t* base = static_cast<uint8_t*>(alloc) + hp;
    ::mprotect(base, cr, host::kProtRead | host::kProtWrite);
    thread->CallRetStackBase = base;
    thread->CurrentFrame->State.callret_sp = reinterpret_cast<uint64_t>(base) + cr / 4;
    t.callret_alloc = alloc;
    t.callret_alloc_size = cr + 2 * hp;
    t.fex_thread = thread;
    {
        std::lock_guard<std::mutex> tl(g_fex_threads_mu);
        g_fex_threads.push_back(thread);
    }
    return true;
}

void Kernel::destroy_fex_thread(GuestThread& t) {
    if (t.fex_thread) {
        auto* thread = static_cast<FEXCore::Core::InternalThreadState*>(t.fex_thread);
        {
            std::lock_guard<std::mutex> tl(g_fex_threads_mu);
            g_fex_threads.erase(std::remove(g_fex_threads.begin(), g_fex_threads.end(), thread), g_fex_threads.end());
        }
        g_ctx->DestroyThread(thread);
        t.fex_thread = nullptr;
    }
    if (t.callret_alloc) {
        ::munmap(t.callret_alloc, t.callret_alloc_size);
        t.callret_alloc = nullptr;
    }
}

bool Kernel::start_host_thread(GuestThread& t, std::string& err) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int prc = pthread_create(&t.host, &attr, [](void* p) -> void* {
        Kernel::get().thread_main(static_cast<GuestThread*>(p));
        return nullptr;
    }, &t);
    pthread_attr_destroy(&attr);
    if (prc != 0) {
        err = "pthread_create";
        return false;
    }
    t.host_started = true;
    return true;
}

void Kernel::release_vfork_parent(GuestProcess& p) {
    GuestThread* parent = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu);
        parent = p.vfork_parent;
        p.vfork_parent = nullptr;
    }
    if (parent) {
        parent->vfork_done = true;
        parent->waiter.notify();
    }
}

void Kernel::process_exited(GuestProcess& p, GuestThread& t, int code, int term_signal) {
    p.exit_code = code;
    p.term_signal = term_signal;
    p.end_ms = NowMs();
    p.state = (int)ProcState::Zombie;
    // Other threads of the group blocked in syscalls wake up and exit.
    for (auto& th : p.threads) {
        if (th.get() != &t && !th->exited) th->waiter.notify();
    }
    release_vfork_parent(p);
    GuestProcess* parent = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu);
        parent = p.ppid ? find(p.ppid) : nullptr;
        if (!parent || parent->state.load() != (int)ProcState::Running) p.reaped = true;  // nobody will wait
    }
    if (parent && !p.reaped) {
        parent->child_wq.wake();
        lx::siginfo si {};
        si.si_signo = lx::sigchld;
        si.si_code = term_signal ? lx::cld_killed : lx::cld_exited;
        si.u.chld.pid = p.pid;
        si.u.chld.uid = 1000;
        si.u.chld.status = term_signal ? term_signal : code;
        send_signal(*parent, lx::sigchld, &si);
    }
}

void Kernel::exit_thread(GuestThread& t, void* frame_, int code, bool whole_group, int term_signal) {
    auto* frame = static_cast<FEXCore::Core::CpuStateFrame*>(frame_);
    t.exit_code = code;
    t.exited = true;
    if (t.clear_child_tid && t.proc->mm) {
        // CLONE_CHILD_CLEARTID: *tidptr = 0 and wake the joiner
        uint32_t zero = 0;
        t.proc->mm->copy_in(t.clear_child_tid, &zero, sizeof zero);
        FutexTable::get().wake(reinterpret_cast<uint32_t*>(t.clear_child_tid), 1, ~0u);
        t.clear_child_tid = 0;
    }
    if (whole_group || t.proc->live_threads() == 0) process_exited(*t.proc, t, code, term_signal);
    // Park the guest on a hlt: the dispatcher leaves ExecuteThread.
    frame->State.rip = hlt_page;
}

void Kernel::thread_main(GuestThread* t) {
    FEXCore::Allocator::InitializeThread();
    t_current_proc = t->proc;
    t_current_thread = t;
    t->waiter.interrupted = &waiter_interrupted;
    t->waiter.interrupted_arg = t;
    SetCurrentWaiter(&t->waiter);
    char name[32];
    snprintf(name, sizeof name, "guest %d/%d", t->proc->pid, t->tid);
#ifdef __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
    auto* thread = static_cast<FEXCore::Core::InternalThreadState*>(t->fex_thread);
    Log("kernel: pid %d tid %d start rip=0x%llx rsp=0x%llx", t->proc->pid, t->tid,
        (unsigned long long)thread->CurrentFrame->State.rip,
        (unsigned long long)thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP]);
    rlfex_fault_info fault {};
    struct Ctx {
        FEXCore::Core::InternalThreadState* th;
    } ctx {thread};
    const int rc = rlfex_run_guarded([](void* p) { g_ctx->ExecuteThread(static_cast<Ctx*>(p)->th); }, &ctx, thread, &fault);
    if (rc != 0) {
        const int lsig = linux_signal_of_host(fault.signal);
        Log("kernel: pid %d tid %d FAULT signal=%d code=%d pc=0x%llx%s addr=0x%llx (last block-exit guest rip=0x%llx, "
            "unaligned fixups so far=%llu) -> killed by signal %d",
            t->proc->pid, t->tid, fault.signal, fault.code, (unsigned long long)fault.pc, fault.in_jit ? " (in JIT code)" : "",
            (unsigned long long)fault.addr, (unsigned long long)thread->CurrentFrame->State.rip,
            (unsigned long long)fault.unaligned_fixups, lsig);
        t->exit_code = 128 + lsig;
        t->exited = true;
        process_exited(*t->proc, *t, 128 + lsig, lsig);
        // The FEX thread object is left alone: its state is unknown after a longjmp.
        if (t->proc->live_threads() == 0) reap(*t->proc);
        return;
    }
    if (!t->exited) {
        // hlt reached without exit_group (bare program) — treat as exit(rax)
        exit_thread(*t, thread->CurrentFrame, (int)thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX], false);
    }
    Log("kernel: pid %d tid %d exited code=%d after %llu syscalls, %.1f ms", t->proc->pid, t->tid, t->exit_code,
        (unsigned long long)t->syscalls, NowMs() - t->proc->start_ms);
    destroy_fex_thread(*t);
    if (t->proc->state.load() != (int)ProcState::Running && t->proc->live_threads() == 0) reap(*t->proc);
}

void Kernel::reap(GuestProcess& p) {
    // The table entry stays (ps, exit codes, /proc/<pid>); the memory and the
    // files go now — each exited process would otherwise keep ~72 MB of the
    // ~6.5 GB address-space budget mapped. A forked child that shares its
    // parent's space only drops its reference.
    std::shared_ptr<AddressSpace> mm;
    {
        std::lock_guard<std::mutex> lk(mu);
        mm = std::move(p.mm);
        p.mm.reset();
        p.fds.clear();
    }
    mm.reset();  // unmaps + invalidates translations outside the kernel lock
}

// ------------------------------------------------------------------ loading

int Kernel::load_image(const std::string& exe_in, const std::vector<std::string>& argv_in,
                       const std::vector<std::string>& envp, AddressSpace& mm, ExecLayout& layout, std::string& err,
                       std::string* resolved_exe) {
    std::string exe = exe_in;
    std::vector<std::string> argv = argv_in;
    int e = 0;
    lx::stat st {};
    std::unique_ptr<FileSource> main;
    for (int depth = 0; depth < 2; depth++) {
        main = open_source(exe, e, &st);
        if (!main) {
            err = exe + ": " + ErrnoName(e);
            return -e;
        }
        if ((st.st_mode & lx::s_ifmt) == lx::s_ifdir) {
            err = exe + ": is a directory";
            return -lx::eacces;
        }
        if (!(st.st_mode & 0111)) {
            err = exe + ": not executable";
            return -lx::eacces;
        }
        // "#!interp [arg]" → interp [arg] exe argv[1..]
        char head[256] = {};
        int64_t n = main->pread(head, sizeof head - 1, 0);
        if (n >= 2 && head[0] == '#' && head[1] == '!') {
            std::string line(head + 2, (size_t)n - 2);
            line = line.substr(0, line.find('\n'));
            size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
            size_t j = i;
            while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') j++;
            std::string interp = line.substr(i, j - i);
            while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) j++;
            std::string arg = line.substr(j);
            while (!arg.empty() && (arg.back() == ' ' || arg.back() == '\t' || arg.back() == '\r')) arg.pop_back();
            if (interp.empty() || depth == 1) {
                err = exe + ": bad interpreter line";
                return -lx::enoexec;
            }
            std::vector<std::string> nargv;
            nargv.push_back(interp);
            if (!arg.empty()) nargv.push_back(arg);
            nargv.push_back(exe);
            for (size_t k = 1; k < argv.size(); k++) nargv.push_back(argv[k]);
            argv = std::move(nargv);
            exe = NormalizePath(interp);
            continue;
        }
        break;
    }
    ExecParams params;
    params.argv = argv;
    params.envp = envp;
    params.exec_path = exe;
    params.hwcap = hwcap;
    params.hwcap2 = hwcap2;
    FillRandom(params.random.data(), params.random.size());
    Kernel* self = this;
    OpenFileFn open_fn = [self](const std::string& p) -> std::unique_ptr<FileSource> {
        int oe = 0;
        return self->open_source(NormalizePath(p), oe, nullptr);
    };
    int rc = ElfLoader::Load(mm, *main, params, open_fn, layout, err);
    if (rc < 0) return rc;
    if (resolved_exe) *resolved_exe = exe;
    return 0;
}

// ------------------------------------------------------------------ spawn

int Kernel::spawn(const std::vector<std::string>& argv, const std::vector<std::string>& envp, const std::string& cwd,
                  bool dry_run, std::string& reply) {
    if (argv.empty()) {
        reply = "{\"ok\":false,\"error\":\"empty argv\"}";
        return -lx::einval;
    }
    // The caller is an app (HTTP) thread: FEX's allocator wants per-thread init
    // before CreateThread allocates through it. Idempotent.
    FEXCore::Allocator::InitializeThread();
    if (!vfs_) {
        reply = "{\"ok\":false,\"error\":\"guest filesystem not mounted (vfs-mount first)\"}";
        return -lx::enoent;
    }
    auto proc = std::make_unique<GuestProcess>();
    proc->cwd = NormalizePath(cwd.empty() ? "/" : cwd);
    proc->argv = argv;
    proc->envp = envp;
    proc->dry_run = dry_run;
    proc->start_ms = NowMs();
    proc->mm = std::make_shared<AddressSpace>();
    const std::string exe = argv[0].empty() || argv[0][0] != '/' ? NormalizePath(proc->cwd + "/" + argv[0]) : NormalizePath(argv[0]);

    std::string lerr, resolved;
    int rc = load_image(exe, argv, envp, *proc->mm, proc->layout, lerr, &resolved);
    if (rc < 0) {
        char m[600];
        snprintf(m, sizeof m, "{\"ok\":false,\"stage\":\"load\",\"error\":\"%s\",\"errno\":\"%s\",\"exe\":\"%s\"}",
                 JsonEscape(lerr).c_str(), ErrnoName(-rc), JsonEscape(exe).c_str());
        reply = m;
        return rc;
    }
    proc->exe = resolved;
    auto slash = resolved.rfind('/');
    proc->comm = resolved.substr(slash == std::string::npos ? 0 : slash + 1).substr(0, 15);

    // stdio: capture + log
    GuestProcess* praw = proc.get();
    auto sink = [praw](int fd, std::string_view data) { praw->append_output(fd, data); };
    proc->fds.alloc(std::make_shared<ConsoleFile>(0, sink), false);
    proc->fds.alloc(std::make_shared<ConsoleFile>(1, sink), false);
    proc->fds.alloc(std::make_shared<ConsoleFile>(2, sink), false);

    int pid;
    GuestThread* t;
    {
        std::lock_guard<std::mutex> lk(mu);
        pid = next_id++;
        proc->pid = pid;
        proc->ppid = 0;   // started by the app: nobody waits for it
        proc->pgid = proc->sid = pid;
        auto th = std::make_unique<GuestThread>();
        th->tid = pid;    // main thread: tid == pid
        th->proc = proc.get();
        t = th.get();
        proc->threads.push_back(std::move(th));
        procs.emplace(pid, std::move(proc));
    }
    const std::string layout = ElfLoader::LayoutJson(praw->layout);
    Log("kernel: pid %d exec %s (%s) %s", pid, resolved.c_str(), dry_run ? "dry-run" : "run", layout.c_str());

    if (dry_run) {
        praw->state = (int)ProcState::Zombie;
        praw->reaped = true;
        praw->exit_code = 0;
        praw->end_ms = NowMs();
        char head[160];
        snprintf(head, sizeof head, "{\"ok\":true,\"pid\":%d,\"dry_run\":true,\"vmas\":%zu,\"mapped_bytes\":%llu,", pid,
                 praw->mm->vma_count(), (unsigned long long)praw->mm->mapped_bytes());
        reply = std::string(head) + "\"layout\":" + layout + ",\"maps\":\"" + JsonEscape(praw->mm->maps_text()) + "\"}";
        reap(*praw);
        return pid;
    }

    std::string ferr;
    if (!ensure_fex(ferr)) {
        praw->state = (int)ProcState::Dead;
        praw->reaped = true;
        reply = "{\"ok\":false,\"pid\":" + std::to_string(pid) + ",\"stage\":\"fex\",\"error\":\"" + JsonEscape(ferr) +
                "\",\"layout\":" + layout + "}";
        reap(*praw);
        return -lx::enoexec;
    }
    if (!create_fex_thread(*t, ferr, nullptr)) {
        praw->state = (int)ProcState::Dead;
        praw->reaped = true;
        reply = "{\"ok\":false,\"pid\":" + std::to_string(pid) + ",\"stage\":\"thread\",\"error\":\"" + JsonEscape(ferr) + "\"}";
        reap(*praw);
        return -lx::enomem;
    }
    if (!start_host_thread(*t, ferr)) {
        praw->state = (int)ProcState::Dead;
        praw->reaped = true;
        destroy_fex_thread(*t);
        reply = "{\"ok\":false,\"error\":\"pthread_create\"}";
        reap(*praw);
        return -lx::eagain;
    }
    reply = "{\"ok\":true,\"pid\":" + std::to_string(pid) + ",\"dry_run\":false,\"layout\":" + layout + "}";
    return pid;
}

// ------------------------------------------------------------------ clone / fork / vfork

int64_t Kernel::do_clone(GuestThread& t, void* frame_, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid,
                         uint64_t tls) {
    auto* frame = static_cast<FEXCore::Core::CpuStateFrame*>(frame_);
    GuestProcess& p = *t.proc;
    const bool is_thread = flags & lx::clone_thread;
    if (is_thread && !(flags & lx::clone_vm)) return -lx::einval;

    // The child's CPU state: the parent's, after the syscall, returning 0.
    FEXCore::Core::CPUState st {};
    memcpy(&st, &frame->State, sizeof st);
    st.rip = frame->State.rip + 2;
    st.gregs[FEXCore::X86State::REG_RAX] = 0;
    if (stack) st.gregs[FEXCore::X86State::REG_RSP] = stack;
    if (flags & lx::clone_settls) st.fs_cached = tls;

    if (is_thread) {
        auto th = std::make_unique<GuestThread>();
        GuestThread* traw = th.get();
        {
            std::lock_guard<std::mutex> lk(mu);
            th->tid = next_id++;
            th->proc = &p;
            th->sigmask = t.sigmask;
            th->clear_child_tid = (flags & lx::clone_child_cleartid) ? ctid : 0;
            p.threads.push_back(std::move(th));
        }
        const int32_t tid = traw->tid;
        if ((flags & lx::clone_parent_settid) && ptid) p.mm->copy_in(ptid, &tid, sizeof tid);
        if ((flags & lx::clone_child_settid) && ctid) p.mm->copy_in(ctid, &tid, sizeof tid);
        std::string err;
        if (!create_fex_thread(*traw, err, &st) || !start_host_thread(*traw, err)) {
            Log("kernel: clone thread failed: %s", err.c_str());
            traw->exited = true;
            destroy_fex_thread(*traw);
            return -lx::eagain;
        }
        return tid;
    }

    // ---- fork / vfork: child in our address space, we wait ----
    const bool vfork = (flags & lx::clone_vfork) && (flags & lx::clone_vm);
    auto child = std::make_unique<GuestProcess>();
    GuestProcess* craw = child.get();
    child->ppid = p.pid;
    child->pgid = p.pgid;
    child->sid = p.sid;
    child->exe = p.exe;
    child->comm = p.comm;
    child->cwd = p.cwd;
    child->argv = p.argv;
    child->envp = p.envp;
    child->umask = p.umask;
    memcpy(child->sigactions, p.sigactions, sizeof p.sigactions);
    child->layout = p.layout;
    child->start_ms = NowMs();
    child->mm = p.mm;  // shared until execve
    child->vfork_parent = &t;
    auto th = std::make_unique<GuestThread>();
    GuestThread* traw = th.get();
    th->proc = craw;
    th->sigmask = t.sigmask;
    th->clear_child_tid = (flags & lx::clone_child_cleartid) ? ctid : 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        child->pid = next_id++;
        th->tid = child->pid;
        child->fds.clone_from(p.fds);
        child->threads.push_back(std::move(th));
        procs.emplace(child->pid, std::move(child));
    }
    const int32_t cpid = craw->pid;
    if ((flags & lx::clone_parent_settid) && ptid) p.mm->copy_in(ptid, &cpid, sizeof cpid);
    if (!vfork) p.mm->push_snapshot();  // from here on the child's writes are undone for us
    if ((flags & lx::clone_child_settid) && ctid) p.mm->copy_in(ctid, &cpid, sizeof cpid);
    t.vfork_done = false;
    std::string err;
    if (!create_fex_thread(*traw, err, &st) || !start_host_thread(*traw, err)) {
        Log("kernel: fork failed: %s", err.c_str());
        traw->exited = true;
        destroy_fex_thread(*traw);
        craw->vfork_parent = nullptr;
        craw->state = (int)ProcState::Dead;
        craw->reaped = true;
        if (!vfork) p.mm->pop_snapshot();
        reap(*craw);
        return -lx::eagain;
    }
    Log("kernel: pid %d %s -> child pid %d%s", p.pid, vfork ? "vfork" : "fork", cpid, vfork ? "" : " (snapshot)");
    // Wait until the child execs or exits (uninterruptible, like vfork).
    t.in_vfork_wait = true;
    for (;;) {
        const uint64_t gen = t.waiter.prepare();
        if (t.vfork_done) break;
        t.waiter.wait(gen, 0);
    }
    t.in_vfork_wait = false;
    if (!vfork) p.mm->pop_snapshot();
    return cpid;
}

// ------------------------------------------------------------------ execve

int64_t Kernel::do_execve(GuestThread& t, void* frame_, const std::string& path, std::vector<std::string> argv,
                          std::vector<std::string> envp) {
    auto* frame = static_cast<FEXCore::Core::CpuStateFrame*>(frame_);
    GuestProcess& p = *t.proc;
    if (argv.empty()) argv.push_back(path);
    auto mm = std::make_shared<AddressSpace>();
    ExecLayout layout;
    std::string err, resolved;
    int rc = load_image(path, argv, envp, *mm, layout, err, &resolved);
    if (rc < 0) {
        Log("kernel: pid %d execve %s failed: %s", p.pid, path.c_str(), err.c_str());
        return rc;
    }
    if (p.live_threads() > 1) Log("kernel: pid %d execve with %zu live threads (not stopped yet)", p.pid, p.live_threads());
    // Point of no return: swap the image in.
    std::shared_ptr<AddressSpace> old;
    {
        std::lock_guard<std::mutex> lk(mu);
        old = std::move(p.mm);
        p.mm = mm;
        p.layout = layout;
        p.exe = resolved;
        auto slash = resolved.rfind('/');
        p.comm = resolved.substr(slash == std::string::npos ? 0 : slash + 1).substr(0, 15);
        p.argv = argv;
        p.envp = envp;
        for (auto& sa : p.sigactions) {
            if (sa.handler != lx::sig_ign) sa = lx::sigaction {};  // handlers → SIG_DFL, SIG_IGN stays
        }
        p.fds.close_on_exec();
    }
    t.altstack_flags = lx::ss_disable;
    t.altstack_sp = t.altstack_size = 0;
    t.clear_child_tid = 0;
    t.robust_list = 0;
    release_vfork_parent(p);   // the parent gets its memory back now
    old.reset();               // last owner → unmapped and translations dropped
    // Fresh CPU state for the new program.
    auto& S = frame->State;
    memset(S.gregs, 0, sizeof S.gregs);
    S.rip = layout.entry;
    S.gregs[FEXCore::X86State::REG_RSP] = layout.rsp;
    S.fs_cached = 0;
    S.gs_cached = 0;
    memset(&S.xmm, 0, sizeof S.xmm);
    memset(S.mm, 0, sizeof S.mm);
    S.FCW = 0x37F;
    S.AbridgedFTW = 0;
    S.mxcsr = 0x1F80;
    auto* thread = static_cast<FEXCore::Core::InternalThreadState*>(t.fex_thread);
    g_ctx->SetFlagsFromCompactedEFLAGS(thread, 0x202);
    S.callret_sp = reinterpret_cast<uint64_t>(thread->CallRetStackBase) + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;
    init_segments(S);
    t.no_retval = true;
    Log("kernel: pid %d execve %s %s", p.pid, resolved.c_str(), ElfLoader::LayoutJson(layout).c_str());
    return 0;
}

// ------------------------------------------------------------------ wait4

int64_t Kernel::do_wait4(GuestThread& t, int pid, int32_t* status_out, int options) {
    GuestProcess& p = *t.proc;
    for (;;) {
        uint64_t gen;
        {
            std::lock_guard<std::mutex> lk(mu);
            bool any = false;
            for (auto& [cpid, c] : procs) {
                if (c->ppid != p.pid || c->reaped) continue;
                if (pid > 0 && cpid != pid) continue;
                if (pid == 0 && c->pgid != p.pgid) continue;
                if (pid < -1 && c->pgid != -pid) continue;
                any = true;
                if (c->state.load() == (int)ProcState::Running) continue;
                const int st = c->term_signal ? lx::wstatus_signaled(c->term_signal) : lx::wstatus_exited(c->exit_code);
                if (status_out) *status_out = st;
                if (!(options & lx::wnowait)) c->reaped = true;
                return cpid;
            }
            if (!any) return -lx::echild;
            if (options & lx::wnohang) return 0;
            p.child_wq.add(&t.waiter);
            gen = t.waiter.prepare();
        }
        auto r = t.waiter.wait(gen, 0);
        p.child_wq.remove(&t.waiter);
        if (r == Waiter::Result::Interrupted) {
            t.restartable = true;
            return -lx::eintr;
        }
    }
}

// ------------------------------------------------------------------ status

std::string Kernel::status_json() {
    std::lock_guard<std::mutex> lk(mu);
    std::string s = "{\"fex\":" + std::string(fex_ready ? "true" : "false") + ",\"syscalls_total\":" +
                    std::to_string(total_syscalls.load()) + ",\"processes\":[";
    bool first = true;
    for (auto& [pid, p] : procs) {
        const char* state = p->state == (int)ProcState::Running ? "running" : (p->state == (int)ProcState::Zombie ? "exited" : "dead");
        char buf[800];
        snprintf(buf, sizeof buf,
                 "%s{\"pid\":%d,\"ppid\":%d,\"exe\":\"%s\",\"state\":\"%s\",\"exit_code\":%d,\"term_signal\":%d,\"reaped\":%s,"
                 "\"threads\":%zu,\"live_threads\":%zu,\"vmas\":%zu,\"mapped_bytes\":%llu,\"fds\":%zu,\"syscalls\":%llu,"
                 "\"dry_run\":%s,\"ms\":%.1f}",
                 first ? "" : ",", pid, p->ppid, JsonEscape(p->exe).c_str(), state, p->exit_code, p->term_signal,
                 p->reaped ? "true" : "false", p->threads.size(), p->live_threads(), p->mm ? p->mm->vma_count() : 0,
                 (unsigned long long)(p->mm ? p->mm->mapped_bytes() : 0), p->fds.count(), (unsigned long long)p->syscalls.load(),
                 p->dry_run ? "true" : "false", (p->end_ms > 0 ? p->end_ms : NowMs()) - p->start_ms);
        s += buf;
        first = false;
    }
    s += "]}";
    return s;
}

std::string Kernel::output_of(int pid) {
    std::lock_guard<std::mutex> lk(mu);
    auto* p = find(pid);
    if (!p) return "";
    std::lock_guard<std::mutex> lo(p->out_mu);
    return p->output;
}

int Kernel::kill_all() {
    std::lock_guard<std::mutex> lk(mu);
    int n = 0;
    for (auto& [pid, p] : procs) {
        if (p->state == (int)ProcState::Running) {
            // No async stop yet: flag it and wake blocked threads; each
            // thread exits at its next syscall.
            p->state = (int)ProcState::Dead;
            for (auto& th : p->threads) th->waiter.notify();
            n++;
        }
    }
    return n;
}

}  // namespace rlk
