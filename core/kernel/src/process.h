// process.h — guest processes (thread groups) and the kernel object.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>

#include <condition_variable>

#include "elf_loader.h"
#include "fds.h"
#include "linux_abi.h"
#include "log.h"
#include "mm.h"
#include "overlay.h"
#include "waitq.h"

namespace FEXCore::Context {
class Context;
}

namespace rlk {

struct GuestProcess;

// The guest process/thread whose syscall this host thread is serving
// (nullptr on app threads). Used for /proc/self, FEX's signal deferring and
// the copy-on-write fault hook.
struct GuestThread;
extern thread_local GuestProcess* t_current_proc;
extern thread_local GuestThread* t_current_thread;

// What uname(2), /proc/version and /proc/sys/kernel report.
constexpr const char* kUtsSysname = "Linux";
constexpr const char* kUtsNodename = "rltvos";
constexpr const char* kUtsRelease = "6.1.0-rltvos";
constexpr const char* kUtsVersion = "#1 SMP PREEMPT_DYNAMIC RLtvOS";
constexpr const char* kUtsMachine = "x86_64";

struct GuestThread {
    int tid = 0;
    GuestProcess* proc = nullptr;
    void* fex_thread = nullptr;     // FEXCore::Core::InternalThreadState*
    pthread_t host {};
    bool host_started = false;
    uint64_t clear_child_tid = 0;   // set_tid_address / CLONE_CHILD_CLEARTID
    uint64_t robust_list = 0;
    uint64_t robust_list_len = 0;
    std::atomic<bool> exited {false};
    int exit_code = 0;
    uint64_t syscalls = 0;
    void* callret_alloc = nullptr;
    size_t callret_alloc_size = 0;

    // ---- signals ----
    uint64_t sigmask = 0;           // blocked signals (bit sig-1)
    uint64_t sigpending = 0;        // thread-directed pending signals
    lx::siginfo siginfo_by_sig[lx::nsig + 1] {};
    uint64_t altstack_sp = 0, altstack_size = 0;
    int altstack_flags = lx::ss_disable;

    // ---- blocking / syscall state ----
    Waiter waiter;                  // blocking syscalls sleep here; signals notify it
    uint64_t syscall_nr = 0;        // number of the syscall in progress (restart)
    bool restartable = false;       // the syscall returned -EINTR and may be restarted after a handler
    bool no_handler_restart = false;  // poll/select/epoll: restart only when no handler runs
    bool no_retval = false;         // execve / rt_sigreturn: leave RAX and RIP alone
    bool vfork_done = false;        // set by our forked child when it execs or exits
    bool in_vfork_wait = false;     // not interruptible while waiting for the child
    bool saved_mask_valid = false;  // rt_sigsuspend: restore `saved_mask` after delivery
    uint64_t saved_mask = 0;
};

enum class ProcState { Running, Zombie, Dead };

// A MAP_SHARED|PROT_WRITE mapping of an upper (tmpfs) file: a private copy
// whose bytes go back into the file on munmap/msync/exit.
struct SharedMap {
    uint64_t start = 0, len = 0;
    std::shared_ptr<TmpNode> node;
    uint64_t off = 0;
};

struct GuestProcess {
    int pid = 0, ppid = 0, pgid = 0, sid = 0;
    std::string exe;    // guest path of the executable
    std::string comm;   // basename, max 15 chars
    std::string cwd = "/";
    std::vector<std::string> argv, envp;
    std::shared_ptr<AddressSpace> mm;   // shared with the parent while a forked child has not exec'd
    FdTable fds;
    std::vector<std::unique_ptr<GuestThread>> threads;
    ExecLayout layout;
    std::atomic<int> state {(int)ProcState::Running};
    int exit_code = 0;
    int term_signal = 0;            // killed by this signal (0 = exited normally)
    bool reaped = false;            // status collected by wait4 (or nobody to collect it)
    lx::sigaction sigactions[lx::nsig] {};
    uint64_t sigpending = 0;        // process-directed pending signals
    lx::siginfo siginfo_by_sig[lx::nsig + 1] {};
    uint32_t umask = 022;
    std::mutex out_mu;
    std::string output;   // captured fd 1/2 (last 64 KB)
    std::atomic<uint64_t> syscalls {0};
    double start_ms = 0, end_ms = 0;
    bool dry_run = false;
    WaitQueue child_wq;             // wait4() sleepers
    GuestThread* vfork_parent = nullptr;  // forked child: the suspended parent thread to release
    std::mutex maps_mu;
    std::vector<SharedMap> shared_maps;
    int64_t itimer_deadline_ns = 0;  // ITIMER_REAL (Kernel::timer_mu_)
    int64_t itimer_interval_ns = 0;

    void append_output(int fd, std::string_view data);
    size_t live_threads() const;
};

class Kernel {
public:
    static Kernel& get();

    // ---- configuration (rlkernel.cpp) ----
    void set_vfs(void* rlvfs_vfs) { vfs_ = rlvfs_vfs; }
    void* vfs() const { return vfs_; }

    // ---- process lifecycle ----
    // Creates a process from argv[0] (guest path), loads it; runs it on a new
    // host thread unless dry_run. Fills reply (JSON). Returns pid or -errno.
    int spawn(const std::vector<std::string>& argv, const std::vector<std::string>& envp, const std::string& cwd,
              bool dry_run, std::string& reply);
    std::string status_json();
    std::string output_of(int pid);
    int kill_all();
    GuestProcess* find(int pid);   // caller holds mu
    void reap(GuestProcess& p);    // frees memory + files of an exited process

    // clone(2) for threads, fork (SIGCHLD only) and vfork (CLONE_VM|CLONE_VFORK).
    // Forks run vfork-style: the child runs in the parent's address space
    // while the parent waits, behind a copy-on-write snapshot that is undone
    // when the child execs or exits. Returns the child's id or -errno.
    int64_t do_clone(GuestThread& t, void* frame, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid,
                     uint64_t tls);
    // execve(2): loads the new image into a fresh address space and resets
    // the calling thread's CPU state. Returns 0 (t.no_retval set) or -errno
    // with the old image intact.
    int64_t do_execve(GuestThread& t, void* frame, const std::string& path, std::vector<std::string> argv,
                      std::vector<std::string> envp);
    int64_t do_wait4(GuestThread& t, int pid, int32_t* status_out, int options);
    void release_vfork_parent(GuestProcess& p);

    // ---- guest filesystem (rlkernel.cpp: the overlay over rlvfs + /proc + /dev) ----
    // Absolute normalized guest path in; positive errno out.
    std::unique_ptr<FileSource> open_source(const std::string& path, int& err, lx::stat* st);
    int stat_path(const std::string& path, bool follow, lx::stat& st);
    int readlink_path(const std::string& path, std::string& target);
    int readdir_path(const std::string& path, std::vector<DirEntryInfo>& out);
    // Resolves a (dirfd, path) pair to an absolute normalized guest path.
    std::string resolve_path(GuestProcess& p, int dirfd, std::string_view path);
    // The writable layer (created on first use with the boot directories).
    Overlay& overlay();

    // ---- shared file mappings (syscalls.cpp / process.cpp) ----
    void add_shared_map(GuestProcess& p, uint64_t start, uint64_t len, std::shared_ptr<TmpNode> node, uint64_t off);
    // Copies the mapped bytes intersecting [addr, addr+len) back into their
    // files; drop: forget those mappings too (munmap, exit, execve).
    void writeback_shared(GuestProcess& p, uint64_t addr, uint64_t len, bool drop);

    // ---- interval timers (timers.cpp) ----
    // ITIMER_REAL: value 0 disarms. Old values (remaining time, interval) out.
    void set_itimer(GuestProcess& p, int64_t value_ns, int64_t interval_ns, int64_t* old_value_ns, int64_t* old_interval_ns);
    void get_itimer(GuestProcess& p, int64_t* value_ns, int64_t* interval_ns);

    // ---- FEX ----
    bool ensure_fex(std::string& err);
    FEXCore::Context::Context* fex_context();
    void handle_syscall(GuestThread& t, void* cpu_state_frame);  // syscalls.cpp
    void thread_main(GuestThread* t);                            // host thread body
    // Ends the calling thread (whole_group: the process) with exit code
    // `code`; term_signal != 0 records death by that signal for wait4.
    void exit_thread(GuestThread& t, void* frame, int code, bool whole_group, int term_signal = 0);
    static bool host_fault_hook(int sig, int code, uint64_t addr, uint64_t pc);

    // ---- signals (signals.cpp) ----
    // Queue a signal for a process / a thread (nullptr info = SI_USER from
    // the caller). Ignored signals are discarded here, like Linux does.
    int send_signal(GuestProcess& p, int sig, const lx::siginfo* info);
    int send_signal_thread(GuestThread& t, int sig, const lx::siginfo* info);
    bool has_deliverable(GuestThread& t) const;
    // End of a syscall (RAX/RIP already set): delivers one pending signal,
    // restarting the interrupted syscall when the action asks for it.
    void deliver_signals(GuestThread& t, void* frame, int64_t syscall_ret);
    int64_t sigreturn(GuestThread& t, void* frame);
    // Blocks the calling thread until a deliverable signal arrives (pause,
    // rt_sigsuspend): returns -EINTR.
    int64_t wait_for_signal(GuestThread& t);

    // ---- log ----
    void strace(GuestThread& t, const char* name, const char* args, int64_t ret);
    void log(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    std::atomic<uint64_t> total_syscalls {0};
    std::atomic<bool> fex_ready {false};
    std::mutex mu;
    std::mutex sig_mu;             // pending bits and siginfo
    std::map<int, std::unique_ptr<GuestProcess>> procs;
    int next_id = 1;               // pids and tids share the number space (main thread tid == pid)
    uint64_t hlt_page = 0;
    uint64_t hwcap = 0, hwcap2 = 0;
    const double boot_ms = NowMs();   // /proc/uptime, btime

private:
    Kernel() = default;
    void* vfs_ = nullptr;
    std::mutex timer_mu_;
    std::condition_variable timer_cv_;
    bool timer_thread_started_ = false;
    void timer_thread_main();
    bool create_fex_thread(GuestThread& t, std::string& err, const void* initial_state);
    void destroy_fex_thread(GuestThread& t);
    bool start_host_thread(GuestThread& t, std::string& err);
    int load_image(const std::string& exe, const std::vector<std::string>& argv, const std::vector<std::string>& envp,
                   AddressSpace& mm, ExecLayout& layout, std::string& err, std::string* resolved_exe);
    void process_exited(GuestProcess& p, GuestThread& t, int code, int term_signal);
};

}  // namespace rlk
