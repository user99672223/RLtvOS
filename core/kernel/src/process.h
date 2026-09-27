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

#include "elf_loader.h"
#include "fds.h"
#include "linux_abi.h"
#include "log.h"
#include "mm.h"

namespace rlk {

struct GuestProcess;

// The guest process/thread whose syscall this host thread is serving
// (nullptr on app threads). Used for /proc/self and FEX's signal deferring.
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
    uint64_t sigmask = 0;
};

enum class ProcState { Running, Zombie, Dead };

struct GuestProcess {
    int pid = 0, ppid = 0, pgid = 0, sid = 0;
    std::string exe;    // guest path of the executable
    std::string comm;   // basename, max 15 chars
    std::string cwd = "/";
    std::vector<std::string> argv, envp;
    std::shared_ptr<AddressSpace> mm;
    FdTable fds;
    std::vector<std::unique_ptr<GuestThread>> threads;
    ExecLayout layout;
    std::atomic<int> state {(int)ProcState::Running};
    int exit_code = 0;
    lx::sigaction sigactions[lx::nsig] {};
    uint32_t umask = 022;
    std::mutex out_mu;
    std::string output;   // captured fd 1/2 (last 64 KB)
    std::atomic<uint64_t> syscalls {0};
    double start_ms = 0, end_ms = 0;
    bool dry_run = false;

    void append_output(int fd, std::string_view data);
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

    // ---- guest filesystem (rlkernel.cpp, over rlvfs) ----
    // Absolute canonical guest path in; -errno out.
    std::unique_ptr<FileSource> open_source(const std::string& path, int& err, lx::stat* st);
    int stat_path(const std::string& path, bool follow, lx::stat& st);
    int readlink_path(const std::string& path, std::string& target);
    int readdir_path(const std::string& path, std::vector<DirEntryInfo>& out);
    // Resolves a (dirfd, path) pair to an absolute normalized guest path.
    std::string resolve_path(GuestProcess& p, int dirfd, std::string_view path);

    // ---- FEX ----
    bool ensure_fex(std::string& err);
    void handle_syscall(GuestThread& t, void* cpu_state_frame);  // syscalls.cpp
    void thread_main(GuestThread* t);                            // host thread body
    void exit_thread(GuestThread& t, void* frame, int code, bool whole_group);

    // ---- log ----
    void strace(GuestThread& t, const char* name, const char* args, int64_t ret);
    void log(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    std::atomic<uint64_t> total_syscalls {0};
    std::atomic<bool> fex_ready {false};
    std::mutex mu;
    std::map<int, std::unique_ptr<GuestProcess>> procs;
    int next_pid = 1, next_tid = 1;
    uint64_t hlt_page = 0;
    uint64_t hwcap = 0, hwcap2 = 0;
    const double boot_ms = NowMs();   // /proc/uptime, btime

private:
    Kernel() = default;
    void* vfs_ = nullptr;
    bool create_fex_thread(GuestThread& t, std::string& err);
    void destroy_fex_thread(GuestThread& t);
};

}  // namespace rlk
