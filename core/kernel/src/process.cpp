// process.cpp — process/thread lifecycle and the FEXCore glue.
// FEXCore and host headers come first: linux_abi.h (via process.h) removes
// the host macros they use inline.
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
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

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

}  // namespace

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
    }
    fex_ready = true;
    Log("kernel: FEX context ready (hwcap=0x%llx hwcap2=0x%llx)", (unsigned long long)hwcap, (unsigned long long)hwcap2);
    return true;
}

bool Kernel::create_fex_thread(GuestThread& t, std::string& err) {
    FEXCore::Core::CPUState st {};
    st.rip = t.proc->layout.entry;
    st.gregs[FEXCore::X86State::REG_RSP] = t.proc->layout.rsp;
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

void Kernel::exit_thread(GuestThread& t, void* frame_, int code, bool whole_group) {
    auto* frame = static_cast<FEXCore::Core::CpuStateFrame*>(frame_);
    t.exit_code = code;
    t.exited = true;
    if (t.clear_child_tid) {
        // CLONE_CHILD_CLEARTID: *tidptr = 0 (+ futex wake, no waiters yet)
        *reinterpret_cast<uint32_t*>(t.clear_child_tid) = 0;
    }
    if (whole_group) {
        t.proc->exit_code = code;
        t.proc->end_ms = NowMs();
        t.proc->state = (int)ProcState::Zombie;
    }
    // Park the guest on a hlt: the dispatcher leaves ExecuteThread.
    frame->State.rip = hlt_page;
}

void Kernel::thread_main(GuestThread* t) {
    FEXCore::Allocator::InitializeThread();
    t_current_proc = t->proc;
    t_current_thread = t;
    char name[32];
    snprintf(name, sizeof name, "guest %d/%d", t->proc->pid, t->tid);
#ifdef __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
    Log("kernel: pid %d tid %d start rip=0x%llx rsp=0x%llx", t->proc->pid, t->tid,
        (unsigned long long)t->proc->layout.entry, (unsigned long long)t->proc->layout.rsp);
    auto* thread = static_cast<FEXCore::Core::InternalThreadState*>(t->fex_thread);
    rlfex_fault_info fault {};
    struct Ctx {
        FEXCore::Core::InternalThreadState* th;
    } ctx {thread};
    const int rc = rlfex_run_guarded([](void* p) { g_ctx->ExecuteThread(static_cast<Ctx*>(p)->th); }, &ctx, thread, &fault);
    if (rc != 0) {
        Log("kernel: pid %d tid %d FAULT signal=%d code=%d pc=0x%llx%s addr=0x%llx (last block-exit guest rip=0x%llx, "
            "unaligned fixups so far=%llu)",
            t->proc->pid, t->tid, fault.signal, fault.code, (unsigned long long)fault.pc, fault.in_jit ? " (in JIT code)" : "",
            (unsigned long long)fault.addr, (unsigned long long)thread->CurrentFrame->State.rip,
            (unsigned long long)fault.unaligned_fixups);
        t->exit_code = 128 + fault.signal;
        t->proc->exit_code = t->exit_code;
        t->proc->end_ms = NowMs();
        t->proc->state = (int)ProcState::Zombie;
        t->exited = true;
        // The FEX thread object is left alone: its state is unknown after a longjmp.
        reap(*t->proc);
        return;
    }
    if (!t->exited) {
        // hlt reached without exit_group (bare program) — treat as exit(rax)
        t->exit_code = (int)thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX];
        t->exited = true;
        t->proc->exit_code = t->exit_code;
        t->proc->end_ms = NowMs();
        t->proc->state = (int)ProcState::Zombie;
    }
    Log("kernel: pid %d tid %d exited code=%d after %llu syscalls, %.1f ms", t->proc->pid, t->tid, t->exit_code,
        (unsigned long long)t->syscalls, t->proc->end_ms - t->proc->start_ms);
    destroy_fex_thread(*t);
    reap(*t->proc);
}

void Kernel::reap(GuestProcess& p) {
    // The table entry stays (ps, exit codes, /proc/<pid>); the memory and the
    // files go now — each exited process would otherwise keep ~72 MB of the
    // ~6.5 GB address-space budget mapped.
    std::shared_ptr<AddressSpace> mm;
    {
        std::lock_guard<std::mutex> lk(mu);
        mm = std::move(p.mm);
        p.mm.reset();
        p.fds.clear();
    }
    mm.reset();  // unmaps + invalidates translations outside the kernel lock
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
    proc->exe = exe;
    auto slash = exe.rfind('/');
    proc->comm = exe.substr(slash == std::string::npos ? 0 : slash + 1).substr(0, 15);

    int err = 0;
    lx::stat st {};
    auto main = open_source(exe, err, &st);
    if (!main) {
        char m[300];
        snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s: %s\"}", JsonEscape(exe).c_str(), ErrnoName(err));
        reply = m;
        return -err;
    }
    if ((st.st_mode & lx::s_ifmt) == lx::s_ifdir) {
        reply = "{\"ok\":false,\"error\":\"is a directory\"}";
        return -lx::eacces;
    }

    ExecParams params;
    params.argv = argv;
    params.envp = envp;
    params.exec_path = exe;
    params.hwcap = hwcap;
    params.hwcap2 = hwcap2;
    FillRandom(params.random.data(), params.random.size());
    std::string lerr;
    Kernel* self = this;
    OpenFileFn open_fn = [self](const std::string& p) -> std::unique_ptr<FileSource> {
        int e = 0;
        return self->open_source(NormalizePath(p), e, nullptr);
    };
    int rc = ElfLoader::Load(*proc->mm, *main, params, open_fn, proc->layout, lerr);
    if (rc < 0) {
        char m[600];
        snprintf(m, sizeof m, "{\"ok\":false,\"stage\":\"load\",\"error\":\"%s\",\"errno\":\"%s\",\"exe\":\"%s\"}",
                 JsonEscape(lerr).c_str(), ErrnoName(-rc), JsonEscape(exe).c_str());
        reply = m;
        return rc;
    }

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
        pid = next_pid++;
        proc->pid = pid;
        proc->ppid = pid == 1 ? 0 : 1;
        proc->pgid = proc->sid = pid;
        auto th = std::make_unique<GuestThread>();
        th->tid = next_tid++;
        th->proc = proc.get();
        t = th.get();
        proc->threads.push_back(std::move(th));
        procs.emplace(pid, std::move(proc));
    }
    const std::string layout = ElfLoader::LayoutJson(praw->layout);
    Log("kernel: pid %d exec %s (%s) %s", pid, exe.c_str(), dry_run ? "dry-run" : "run", layout.c_str());

    if (dry_run) {
        praw->state = (int)ProcState::Zombie;
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
        reply = "{\"ok\":false,\"pid\":" + std::to_string(pid) + ",\"stage\":\"fex\",\"error\":\"" + JsonEscape(ferr) +
                "\",\"layout\":" + layout + "}";
        return -lx::enoexec;
    }
    if (!create_fex_thread(*t, ferr)) {
        praw->state = (int)ProcState::Dead;
        reply = "{\"ok\":false,\"pid\":" + std::to_string(pid) + ",\"stage\":\"thread\",\"error\":\"" + JsonEscape(ferr) + "\"}";
        return -lx::enomem;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int prc = pthread_create(&t->host, &attr, [](void* p) -> void* {
        Kernel::get().thread_main(static_cast<GuestThread*>(p));
        return nullptr;
    }, t);
    pthread_attr_destroy(&attr);
    if (prc != 0) {
        praw->state = (int)ProcState::Dead;
        destroy_fex_thread(*t);
        reply = "{\"ok\":false,\"error\":\"pthread_create\"}";
        return -lx::eagain;
    }
    t->host_started = true;
    reply = "{\"ok\":true,\"pid\":" + std::to_string(pid) + ",\"dry_run\":false,\"layout\":" + layout + "}";
    return pid;
}

std::string Kernel::status_json() {
    std::lock_guard<std::mutex> lk(mu);
    std::string s = "{\"fex\":" + std::string(fex_ready ? "true" : "false") + ",\"syscalls_total\":" +
                    std::to_string(total_syscalls.load()) + ",\"processes\":[";
    bool first = true;
    for (auto& [pid, p] : procs) {
        const char* state = p->state == (int)ProcState::Running ? "running" : (p->state == (int)ProcState::Zombie ? "exited" : "dead");
        char buf[700];
        snprintf(buf, sizeof buf,
                 "%s{\"pid\":%d,\"ppid\":%d,\"exe\":\"%s\",\"state\":\"%s\",\"exit_code\":%d,\"threads\":%zu,\"vmas\":%zu,"
                 "\"mapped_bytes\":%llu,\"fds\":%zu,\"syscalls\":%llu,\"dry_run\":%s,\"ms\":%.1f}",
                 first ? "" : ",", pid, p->ppid, JsonEscape(p->exe).c_str(), state, p->exit_code, p->threads.size(),
                 p->mm ? p->mm->vma_count() : 0, (unsigned long long)(p->mm ? p->mm->mapped_bytes() : 0), p->fds.count(),
                 (unsigned long long)p->syscalls.load(), p->dry_run ? "true" : "false",
                 (p->end_ms > 0 ? p->end_ms : NowMs()) - p->start_ms);
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
            // No async stop yet (needs the signal delegator): flag it; the
            // next syscall of that process exits it.
            p->state = (int)ProcState::Dead;
            n++;
        }
    }
    return n;
}

}  // namespace rlk
