// rlkernel.cpp — the C API (rlkernel.h) and the guest filesystem bridge: the
// read-only rlvfs tree served from the laptop plus synthetic /proc and /dev
// entries. Paths arriving here are absolute and lexically normalized
// (Kernel::resolve_path); symlinks are resolved by the VFS at lookup.
// Host / rlvfs headers first (see linux_abi.h on macro hygiene).
#include "rlvfs/vfs.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rlkernel/rlkernel.h"

#include "fds.h"
#include "linux_abi.h"
#include "log.h"
#include "overlay.h"
#include "process.h"

namespace rlk {

namespace {

// rlvfs reports host errno values; the guest wants Linux numbering.
inline int HostErrno(int e) {
    return host::ErrnoToLinux(e);
}

// ---- rlvfs bridge ----------------------------------------------------------------

// rlvfs' block cache has its own locks, but the tree walk and the HTTP client
// were only exercised single-threaded so far: one lock around every call
// until that is verified (C4/C5 will show whether it matters).
std::mutex g_vfs_mu;

rlvfs::Vfs* vfs_of(Kernel& k) {
    return static_cast<rlvfs::Vfs*>(k.vfs());
}

class VfsFileSource final : public FileSource {
public:
    VfsFileSource(rlvfs::Vfs* vfs, const rlvfs::Entry* e, std::string guest_path)
        : vfs_(vfs), e_(e), path_(std::move(guest_path)), ino_(rlvfs::Vfs::inode_for(e->path)) {}
    uint64_t size() const override { return e_->size; }
    int64_t pread(void* buf, size_t len, uint64_t off) override {
        if (len == 0) return 0;
        std::string err;
        std::lock_guard<std::mutex> lk(g_vfs_mu);
        int64_t n = vfs_->pread(*e_, buf, len, off, err);
        if (n < 0) {
            Log("vfs: pread %s off=%llu len=%zu failed: %s", path_.c_str(), (unsigned long long)off, len, err.c_str());
            return -HostErrno((int)-n);
        }
        return n;
    }
    const std::string& path() const override { return path_; }
    uint64_t inode() const override { return ino_; }

private:
    rlvfs::Vfs* vfs_;
    const rlvfs::Entry* e_;
    std::string path_;
    uint64_t ino_;
};

void to_lx_stat(const rlvfs::GuestStat& g, lx::stat& st) {
    FillStat(st, g.mode, g.size, g.mtime, g.ino, g.rdev, g.uid, g.gid);
    st.st_nlink = g.nlink;
}

// ---- /proc -------------------------------------------------------------------------

struct ProcNode {
    enum Kind { None, Dir, File, Link } kind = None;
    GuestProcess* proc = nullptr;  // per-process node (nullptr = global)
    std::string name;              // "maps", "fd", "sys/kernel/osrelease", "" = the dir itself
    std::string target;            // Link
    int fd = -1;                   // fd/N links
};

constexpr const char* kPidFiles[] = {"maps", "status", "stat", "statm", "cmdline", "environ", "auxv",
                                     "comm", "mountinfo", "mounts", "limits", "cgroup", "oom_score_adj"};
constexpr const char* kGlobalFiles[] = {"cpuinfo", "meminfo", "version", "uptime", "loadavg", "filesystems",
                                        "mounts", "stat", "cmdline"};
constexpr const char* kSysDirs[] = {"sys", "sys/kernel", "sys/kernel/random", "sys/vm", "sys/fs"};
constexpr const char* kSysFiles[] = {"sys/kernel/osrelease", "sys/kernel/ostype", "sys/kernel/version",
                                     "sys/kernel/hostname", "sys/kernel/pid_max", "sys/kernel/ngroups_max",
                                     "sys/kernel/cap_last_cap", "sys/kernel/random/boot_id",
                                     "sys/kernel/random/uuid", "sys/vm/overcommit_memory", "sys/vm/max_map_count",
                                     "sys/vm/mmap_min_addr", "sys/fs/file-max", "sys/fs/nr_open"};

// Vfs::inode_for is a string hash: stable, unique enough for /proc nodes.
uint64_t proc_ino(const std::string& path) {
    uint64_t i = rlvfs::Vfs::inode_for(path);
    return i ? i : 1;
}

bool in_list(const char* const* list, size_t n, const std::string& s) {
    for (size_t i = 0; i < n; i++) {
        if (s == list[i]) return true;
    }
    return false;
}
#define IN_LIST(list, s) in_list(list, sizeof(list) / sizeof(list[0]), s)

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// "self"/"thread-self" → the calling guest; "<pid>" → that process.
GuestProcess* pid_lookup(Kernel& k, std::string_view comp) {
    if (comp == "self" || comp == "thread-self") return t_current_proc;
    if (!all_digits(comp) || comp.size() > 7) return nullptr;
    int pid = atoi(std::string(comp).c_str());
    if (pid <= 0) return nullptr;
    std::lock_guard<std::mutex> lk(k.mu);
    return k.find(pid);
}

// Nodes under /proc/<pid>/ (tail = path after the pid component).
bool pid_node(GuestProcess& p, const std::string& tail, ProcNode& n) {
    n.proc = &p;
    if (tail.empty()) {
        n.kind = ProcNode::Dir;
        return true;
    }
    size_t slash = tail.find('/');
    std::string name = tail.substr(0, slash);
    std::string sub = slash == std::string::npos ? "" : tail.substr(slash + 1);
    n.name = name;
    if (sub.empty() && IN_LIST(kPidFiles, name)) {
        n.kind = ProcNode::File;
        return true;
    }
    if (sub.empty() && (name == "exe" || name == "cwd" || name == "root")) {
        n.kind = ProcNode::Link;
        n.target = name == "exe" ? p.exe : (name == "cwd" ? p.cwd : "/");
        return true;
    }
    if (name == "fd") {
        if (sub.empty()) {
            n.kind = ProcNode::Dir;
            return true;
        }
        if (!all_digits(sub) || sub.size() > 6) {
            n.kind = ProcNode::None;
            return true;
        }
        int fd = atoi(sub.c_str());
        auto f = p.fds.get(fd);
        if (!f) {
            n.kind = ProcNode::None;
            return true;
        }
        n.kind = ProcNode::Link;
        n.fd = fd;
        n.target = f->path;
        return true;
    }
    if (name == "task") {
        if (sub.empty()) {
            n.kind = ProcNode::Dir;
            return true;
        }
        // task/<tid>[/file]: every thread shows the process's files
        size_t s2 = sub.find('/');
        std::string tid = sub.substr(0, s2);
        std::string rest = s2 == std::string::npos ? "" : sub.substr(s2 + 1);
        if (!all_digits(tid)) {
            n.kind = ProcNode::None;
            return true;
        }
        if (rest.empty()) {
            n.kind = ProcNode::Dir;
            n.name = "task/" + tid;
            return true;
        }
        return pid_node(p, rest, n);
    }
    n.kind = ProcNode::None;
    return true;
}

// Global /proc nodes ("cpuinfo", "sys/kernel/osrelease", ...).
bool global_node(const std::string& rel, ProcNode& n) {
    n.name = rel;
    if (IN_LIST(kGlobalFiles, rel) || IN_LIST(kSysFiles, rel)) {
        n.kind = ProcNode::File;
        return true;
    }
    if (IN_LIST(kSysDirs, rel)) {
        n.kind = ProcNode::Dir;
        return true;
    }
    n.kind = ProcNode::None;
    return true;
}

// true when `path` is under /proc (n then describes it, possibly None).
bool proc_lookup(Kernel& k, const std::string& path, ProcNode& n) {
    if (path != "/proc" && path.rfind("/proc/", 0) != 0) return false;
    n = ProcNode {};
    if (path == "/proc") {
        n.kind = ProcNode::Dir;
        return true;
    }
    const std::string rest = path.substr(6);
    size_t slash = rest.find('/');
    const std::string first = rest.substr(0, slash);
    const std::string tail = slash == std::string::npos ? "" : rest.substr(slash + 1);
    if (first == "self" || first == "thread-self") {
        GuestProcess* p = t_current_proc;
        if (!p) {
            n.kind = ProcNode::None;
            return true;
        }
        if (tail.empty()) {
            n.kind = ProcNode::Link;
            n.proc = p;
            n.target = std::to_string(p->pid);
            return true;
        }
        return pid_node(*p, tail, n);
    }
    if (all_digits(first)) {
        GuestProcess* p = pid_lookup(k, first);
        if (!p) {
            n.kind = ProcNode::None;
            return true;
        }
        return pid_node(*p, tail, n);
    }
    return global_node(rest, n);
}

constexpr const char* kCpuFlags =
    "fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ht syscall nx "
    "lm constant_tsc nopl pni pclmulqdq ssse3 cx16 sse4_1 sse4_2 movbe popcnt aes xsave rdrand lahf_lm abm fsgsbase "
    "bmi1 bmi2 rdseed";

unsigned cpu_count() {
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1;
}

std::string gen_global(Kernel& k, const std::string& name) {
    char buf[1024];
    if (name == "cpuinfo") {
        std::string s;
        const unsigned n = cpu_count();
        for (unsigned i = 0; i < n; i++) {
            snprintf(buf, sizeof buf,
                     "processor\t: %u\nvendor_id\t: GenuineIntel\ncpu family\t: 6\nmodel\t\t: 158\n"
                     "model name\t: RLtvOS x86-64 (FEX on Apple A15)\nstepping\t: 10\nmicrocode\t: 0x1\n"
                     "cpu MHz\t\t: 2000.000\ncache size\t: 8192 KB\nphysical id\t: 0\nsiblings\t: %u\ncore id\t\t: %u\n"
                     "cpu cores\t: %u\napicid\t\t: %u\ninitial apicid\t: %u\nfpu\t\t: yes\nfpu_exception\t: yes\n"
                     "cpuid level\t: 22\nwp\t\t: yes\nflags\t\t: %s\nbugs\t\t:\nbogomips\t: 4000.00\nclflush size\t: 64\n"
                     "cache_alignment\t: 64\naddress sizes\t: 48 bits physical, 48 bits virtual\npower management:\n\n",
                     i, n, i, n, i, i, kCpuFlags);
            s += buf;
        }
        return s;
    }
    if (name == "meminfo") {
        // The jetsam budget is ~2 GB; report that as the machine.
        snprintf(buf, sizeof buf,
                 "MemTotal:       %8llu kB\nMemFree:        %8llu kB\nMemAvailable:   %8llu kB\nBuffers:               0 kB\n"
                 "Cached:                0 kB\nSwapCached:            0 kB\nActive:                0 kB\nInactive:              0 kB\n"
                 "SwapTotal:             0 kB\nSwapFree:              0 kB\nDirty:                 0 kB\nShmem:                 0 kB\n"
                 "CommitLimit:    %8llu kB\nCommitted_AS:          0 kB\nVmallocTotal:   34359738367 kB\nHugepagesize:       2048 kB\n",
                 2048ull * 1024, 1536ull * 1024, 1536ull * 1024, 2048ull * 1024);
        return buf;
    }
    if (name == "version") {
        snprintf(buf, sizeof buf, "%s version %s (rltvos@rltvos) (clang) %s\n", kUtsSysname, kUtsRelease, kUtsVersion);
        return buf;
    }
    if (name == "uptime") {
        double up = (NowMs() - k.boot_ms) / 1000.0;
        snprintf(buf, sizeof buf, "%.2f %.2f\n", up, up * cpu_count());
        return buf;
    }
    if (name == "loadavg") {
        size_t n;
        {
            std::lock_guard<std::mutex> lk(k.mu);
            n = k.procs.size();
        }
        snprintf(buf, sizeof buf, "0.10 0.05 0.01 1/%zu %d\n", n + 1, k.next_id);
        return buf;
    }
    if (name == "filesystems") return "nodev\tsysfs\nnodev\ttmpfs\nnodev\tproc\nnodev\tdevtmpfs\nnodev\tdevpts\n\text4\n";
    if (name == "mounts") {
        return "rootfs / rootfs ro,relatime 0 0\nproc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
               "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\ndevtmpfs /dev devtmpfs rw,nosuid 0 0\n"
               "tmpfs /dev/shm tmpfs rw,nosuid,nodev 0 0\ntmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n";
    }
    if (name == "stat") {
        std::string s = "cpu  0 0 0 0 0 0 0 0 0 0\n";
        for (unsigned i = 0; i < cpu_count(); i++) {
            snprintf(buf, sizeof buf, "cpu%u 0 0 0 0 0 0 0 0 0 0\n", i);
            s += buf;
        }
        snprintf(buf, sizeof buf, "intr 0\nctxt 0\nbtime %lld\nprocesses %d\nprocs_running 1\nprocs_blocked 0\n",
                 (long long)(k.boot_ms / 1000.0), k.next_id);
        return s + buf;
    }
    if (name == "cmdline") return "BOOT_IMAGE=/vmlinuz root=/dev/rltvos ro quiet\n";
    if (name == "sys/kernel/osrelease") return std::string(kUtsRelease) + "\n";
    if (name == "sys/kernel/ostype") return std::string(kUtsSysname) + "\n";
    if (name == "sys/kernel/version") return std::string(kUtsVersion) + "\n";
    if (name == "sys/kernel/hostname") return std::string(kUtsNodename) + "\n";
    if (name == "sys/kernel/pid_max") return "4194304\n";
    if (name == "sys/kernel/ngroups_max") return "65536\n";
    if (name == "sys/kernel/cap_last_cap") return "40\n";
    if (name == "sys/kernel/random/boot_id") return "9d3b1c1a-2e4f-4c0a-a1f5-b2c3d4e5f607\n";
    if (name == "sys/kernel/random/uuid") {
        uint8_t r[16];
        FillRandom(r, sizeof r);
        snprintf(buf, sizeof buf, "%02x%02x%02x%02x-%02x%02x-4%01x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", r[0], r[1], r[2],
                 r[3], r[4], r[5], r[6] & 15, r[7], (r[8] & 0x3f) | 0x80, r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
        return buf;
    }
    if (name == "sys/vm/overcommit_memory") return "0\n";
    if (name == "sys/vm/max_map_count") return "65530\n";
    if (name == "sys/vm/mmap_min_addr") return "65536\n";
    if (name == "sys/fs/file-max") return "9223372036854775807\n";
    if (name == "sys/fs/nr_open") return "1048576\n";
    return "";
}

std::string gen_pid(Kernel& k, GuestProcess& p, const std::string& name) {
    char buf[2048];
    if (name == "maps") return p.mm ? p.mm->maps_text() : "";
    if (name == "cmdline" || name == "environ") {
        std::string s;
        for (auto& a : name == "cmdline" ? p.argv : p.envp) {
            s += a;
            s.push_back('\0');
        }
        return s;
    }
    if (name == "comm") return p.comm + "\n";
    if (name == "auxv") {
        if (!p.layout.auxv || !p.layout.auxv_entries) return "";
        return std::string(reinterpret_cast<const char*>(p.layout.auxv), p.layout.auxv_entries * 16);
    }
    const uint64_t vm_kb = p.mm ? p.mm->mapped_bytes() / 1024 : 0;
    const bool running = p.state.load() == (int)ProcState::Running;
    if (name == "status") {
        const uint64_t sigblk = p.threads.empty() ? 0 : p.threads[0]->sigmask;
        snprintf(buf, sizeof buf,
                 "Name:\t%s\nUmask:\t%04o\nState:\t%s\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\n"
                 "Uid:\t1000\t1000\t1000\t1000\nGid:\t1000\t1000\t1000\t1000\nFDSize:\t256\nGroups:\t1000 \n"
                 "NStgid:\t%d\nNSpid:\t%d\nNSpgid:\t%d\nNSsid:\t%d\n"
                 "VmPeak:\t%8llu kB\nVmSize:\t%8llu kB\nVmLck:\t       0 kB\nVmPin:\t       0 kB\nVmHWM:\t%8llu kB\n"
                 "VmRSS:\t%8llu kB\nRssAnon:\t%8llu kB\nRssFile:\t       0 kB\nRssShmem:\t       0 kB\nVmData:\t%8llu kB\n"
                 "VmStk:\t    8192 kB\nVmExe:\t       0 kB\nVmLib:\t       0 kB\nVmPTE:\t       0 kB\nVmSwap:\t       0 kB\n"
                 "Threads:\t%zu\nSigQ:\t0/15000\nSigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\n"
                 "SigBlk:\t%016llx\nSigIgn:\t0000000000000000\nSigCgt:\t0000000000000000\n"
                 "CapInh:\t0000000000000000\nCapPrm:\t0000000000000000\nCapEff:\t0000000000000000\n"
                 "CapBnd:\t000001ffffffffff\nCapAmb:\t0000000000000000\nNoNewPrivs:\t0\nSeccomp:\t0\nSeccomp_filters:\t0\n"
                 "Cpus_allowed:\t%x\nCpus_allowed_list:\t0-%u\nMems_allowed:\t1\nMems_allowed_list:\t0\n"
                 "voluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n",
                 p.comm.c_str(), p.umask, running ? "R (running)" : "Z (zombie)", p.pid, p.pid, p.ppid, p.pid, p.pid,
                 p.pgid, p.sid, (unsigned long long)vm_kb, (unsigned long long)vm_kb, (unsigned long long)vm_kb,
                 (unsigned long long)vm_kb, (unsigned long long)vm_kb, (unsigned long long)vm_kb, p.threads.size(),
                 (unsigned long long)sigblk, (1u << cpu_count()) - 1, cpu_count() - 1);
        return buf;
    }
    if (name == "stat") {
        snprintf(buf, sizeof buf,
                 "%d (%s) %c %d %d %d 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 %zu 0 %llu %llu %llu 18446744073709551615 "
                 "0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                 p.pid, p.comm.c_str(), running ? 'R' : 'Z', p.ppid, p.pgid, p.sid, p.threads.size(),
                 (unsigned long long)((p.start_ms - k.boot_ms) / 10.0), (unsigned long long)vm_kb * 1024,
                 (unsigned long long)vm_kb / 4);
        return buf;
    }
    if (name == "statm") {
        snprintf(buf, sizeof buf, "%llu %llu 0 0 0 %llu 0\n", (unsigned long long)vm_kb / 4, (unsigned long long)vm_kb / 4,
                 (unsigned long long)vm_kb / 4);
        return buf;
    }
    if (name == "mountinfo") {
        return "1 0 8:1 / / ro,relatime - rootfs rootfs ro\n2 1 0:5 / /proc rw,nosuid,nodev,noexec,relatime - proc proc rw\n"
               "3 1 0:6 / /dev rw,nosuid - devtmpfs devtmpfs rw\n4 3 0:7 / /dev/shm rw,nosuid,nodev - tmpfs tmpfs rw\n"
               "5 1 0:8 / /tmp rw,nosuid,nodev - tmpfs tmpfs rw\n";
    }
    if (name == "mounts") return gen_global(k, "mounts");
    if (name == "limits") {
        return "Limit                     Soft Limit           Hard Limit           Units     \n"
               "Max cpu time              unlimited            unlimited            seconds   \n"
               "Max file size             unlimited            unlimited            bytes     \n"
               "Max data size             unlimited            unlimited            bytes     \n"
               "Max stack size            8388608              unlimited            bytes     \n"
               "Max core file size        0                    unlimited            bytes     \n"
               "Max resident set          unlimited            unlimited            bytes     \n"
               "Max processes             4096                 4096                 processes \n"
               "Max open files            4096                 4096                 files     \n"
               "Max locked memory         65536                65536                bytes     \n"
               "Max address space         unlimited            unlimited            bytes     \n"
               "Max file locks            unlimited            unlimited            locks     \n"
               "Max pending signals       15000                15000                signals   \n"
               "Max msgqueue size         819200               819200               bytes     \n"
               "Max nice priority         0                    0                    \n"
               "Max realtime priority     0                    0                    \n"
               "Max realtime timeout      unlimited            unlimited            us        \n";
    }
    if (name == "cgroup") return "0::/\n";
    if (name == "oom_score_adj") return "0\n";
    return "";
}

// Directory listing of a synthetic /proc directory.
void proc_list(Kernel& k, const ProcNode& n, std::vector<DirEntryInfo>& out) {
    auto add = [&](const std::string& name, uint8_t dtype) { out.push_back({name, dtype, proc_ino("/proc/" + name)}); };
    if (n.proc) {
        if (n.name == "fd") {
            for (auto& [fd, path] : n.proc->fds.list()) add(std::to_string(fd), lx::dt_lnk);
            return;
        }
        if (n.name == "task") {
            for (auto& t : n.proc->threads) add(std::to_string(t->tid), lx::dt_dir);
            return;
        }
        for (const char* f : kPidFiles) add(f, lx::dt_reg);
        add("exe", lx::dt_lnk);
        add("cwd", lx::dt_lnk);
        add("root", lx::dt_lnk);
        add("fd", lx::dt_dir);
        add("task", lx::dt_dir);
        return;
    }
    if (n.name.empty()) {  // /proc itself
        {
            std::lock_guard<std::mutex> lk(k.mu);
            for (auto& [pid, p] : k.procs) add(std::to_string(pid), lx::dt_dir);
        }
        add("self", lx::dt_lnk);
        add("thread-self", lx::dt_lnk);
        for (const char* f : kGlobalFiles) add(f, lx::dt_reg);
        add("sys", lx::dt_dir);
        return;
    }
    // sys subtree: direct children of n.name
    const std::string prefix = n.name + "/";
    auto child_of = [&](const std::string& full) -> std::string {
        if (full.rfind(prefix, 0) != 0) return "";
        std::string rest = full.substr(prefix.size());
        return rest.find('/') == std::string::npos ? rest : "";
    };
    for (const char* d : kSysDirs) {
        std::string c = child_of(d);
        if (!c.empty()) add(c, lx::dt_dir);
    }
    for (const char* f : kSysFiles) {
        std::string c = child_of(f);
        if (!c.empty()) add(c, lx::dt_reg);
    }
}

// ---- /dev (only what the rootfs image lacks) -----------------------------------------

struct DevNode {
    const char* path;
    uint32_t mode;
    uint64_t rdev;
    const char* link;
};
constexpr DevNode kDevNodes[] = {
    {"/dev/null", lx::s_ifchr | 0666, (1u << 8) | 3, nullptr},
    {"/dev/zero", lx::s_ifchr | 0666, (1u << 8) | 5, nullptr},
    {"/dev/full", lx::s_ifchr | 0666, (1u << 8) | 7, nullptr},
    {"/dev/random", lx::s_ifchr | 0666, (1u << 8) | 8, nullptr},
    {"/dev/urandom", lx::s_ifchr | 0666, (1u << 8) | 9, nullptr},
    {"/dev/tty", lx::s_ifchr | 0666, (5u << 8) | 0, nullptr},
    {"/dev/console", lx::s_ifchr | 0600, (5u << 8) | 1, nullptr},
    {"/dev/ptmx", lx::s_ifchr | 0666, (5u << 8) | 2, nullptr},
    {"/dev/fd", lx::s_iflnk | 0777, 0, "/proc/self/fd"},
    {"/dev/stdin", lx::s_iflnk | 0777, 0, "/proc/self/fd/0"},
    {"/dev/stdout", lx::s_iflnk | 0777, 0, "/proc/self/fd/1"},
    {"/dev/stderr", lx::s_iflnk | 0777, 0, "/proc/self/fd/2"},
    {"/dev/shm", lx::s_ifdir | 01777, 0, nullptr},
    {"/dev/pts", lx::s_ifdir | 0755, 0, nullptr},
};

const DevNode* dev_node(const std::string& path) {
    for (const auto& d : kDevNodes) {
        if (path == d.path) return &d;
    }
    return nullptr;
}

}  // namespace

// ---- Kernel: guest filesystem — the lower (read-only) tree: /proc, rlvfs, /dev fallback ----

std::string Kernel::resolve_path(GuestProcess& p, int dirfd, std::string_view path) {
    if (!path.empty() && path[0] == '/') return NormalizePath(path);
    std::string base = p.cwd;
    if (dirfd != lx::at_fdcwd) {
        auto f = p.fds.get(dirfd);
        if (f) base = f->path;
    }
    if (path.empty()) return NormalizePath(base);
    return NormalizePath(base + "/" + std::string(path));
}

static int lower_stat(Kernel& k, const std::string& path, bool follow, lx::stat& st) {
    ProcNode n;
    if (proc_lookup(k, path, n)) {
        switch (n.kind) {
            case ProcNode::Dir:
                FillStat(st, lx::s_ifdir | 0555, 0, 0, proc_ino(path), 0, 0, 0);
                st.st_nlink = 2;
                return 0;
            case ProcNode::File:
                FillStat(st, lx::s_ifreg | 0444, 0, 0, proc_ino(path), 0, 0, 0);
                return 0;
            case ProcNode::Link:
                if (!follow) {
                    FillStat(st, lx::s_iflnk | 0777, n.target.size(), 0, proc_ino(path), 0, 0, 0);
                    return 0;
                }
                if (n.fd >= 0) {
                    auto f = n.proc->fds.get(n.fd);
                    if (!f) return lx::enoent;
                    int rc = f->fstat(st);
                    return rc < 0 ? -rc : 0;
                }
                if (n.target.empty() || n.target[0] != '/') {  // /proc/self → "<pid>"
                    FillStat(st, lx::s_ifdir | 0555, 0, 0, proc_ino("/proc/" + n.target), 0, 0, 0);
                    return 0;
                }
                return lower_stat(k, n.target, true, st);
            case ProcNode::None: return lx::enoent;
        }
    }
    auto* vfs = vfs_of(k);
    int err = lx::enoent;
    if (vfs) {
        rlvfs::GuestStat g;
        std::lock_guard<std::mutex> lk(g_vfs_mu);
        int rc = vfs->stat(path, follow, g);
        if (rc == 0) {
            to_lx_stat(g, st);
            return 0;
        }
        err = HostErrno(-rc);
    }
    if (const DevNode* d = dev_node(path)) {
        if ((d->mode & lx::s_ifmt) == lx::s_iflnk && follow) return lower_stat(k, d->link, true, st);
        FillStat(st, d->mode, d->link ? strlen(d->link) : 0, 0, proc_ino(path), d->rdev, 0, 0);
        return 0;
    }
    return err;
}

static std::unique_ptr<FileSource> lower_open(Kernel& k, const std::string& path, int& err, lx::stat* st) {
    ProcNode n;
    if (proc_lookup(k, path, n)) {
        if (n.kind == ProcNode::Link) {
            if (n.target.empty() || n.target[0] != '/') {
                err = lx::eisdir;
                return nullptr;
            }
            return lower_open(k, n.target, err, st);
        }
        if (n.kind == ProcNode::File) {
            std::string content = n.proc ? gen_pid(k, *n.proc, n.name) : gen_global(k, n.name);
            if (st) FillStat(*st, lx::s_ifreg | 0444, 0, 0, proc_ino(path), 0, 0, 0);
            return std::make_unique<MemFileSource>(path, std::vector<uint8_t>(content.begin(), content.end()));
        }
        err = n.kind == ProcNode::Dir ? lx::eisdir : lx::enoent;
        return nullptr;
    }
    auto* vfs = vfs_of(k);
    if (!vfs) {
        err = lx::enoent;
        return nullptr;
    }
    int herr = 0;
    std::string canon;
    const rlvfs::Entry* e;
    {
        std::lock_guard<std::mutex> lk(g_vfs_mu);
        e = vfs->resolve(path, true, &herr, &canon);
    }
    if (!e) {
        err = HostErrno(herr);
        return nullptr;
    }
    if (e->is_dir()) {
        err = lx::eisdir;
        return nullptr;
    }
    if (!e->is_file()) {
        err = lx::enxio;
        return nullptr;
    }
    if (st) {
        rlvfs::GuestStat g;
        rlvfs::Vfs::stat_entry(*e, g);
        to_lx_stat(g, *st);
    }
    return std::make_unique<VfsFileSource>(vfs, e, canon.empty() ? path : canon);
}

static int lower_readlink(Kernel& k, const std::string& path, std::string& target) {
    ProcNode n;
    if (proc_lookup(k, path, n)) {
        if (n.kind == ProcNode::Link) {
            target = n.target;
            return 0;
        }
        return n.kind == ProcNode::None ? lx::enoent : lx::einval;
    }
    auto* vfs = vfs_of(k);
    int err = lx::enoent;
    if (vfs) {
        std::lock_guard<std::mutex> lk(g_vfs_mu);
        int rc = vfs->readlink(path, target);
        if (rc == 0) return 0;
        err = HostErrno(-rc);
    }
    if (const DevNode* d = dev_node(path)) {
        if (!d->link) return lx::einval;
        target = d->link;
        return 0;
    }
    return err;
}

static int lower_readdir(Kernel& k, const std::string& path, std::vector<DirEntryInfo>& out) {
    ProcNode n;
    if (proc_lookup(k, path, n)) {
        if (n.kind == ProcNode::Link && n.proc && (n.target.empty() || n.target[0] != '/')) {
            // /proc/self as a directory
            n.kind = ProcNode::Dir;
            n.name = "";
        }
        if (n.kind != ProcNode::Dir) return n.kind == ProcNode::None ? lx::enoent : lx::enotdir;
        out.push_back({".", lx::dt_dir, proc_ino(path)});
        out.push_back({"..", lx::dt_dir, 1});
        proc_list(k, n, out);
        return 0;
    }
    auto* vfs = vfs_of(k);
    int rc = -1;
    int err = lx::enoent;
    if (vfs) {
        std::vector<rlvfs::DirEnt> ents;
        {
            std::lock_guard<std::mutex> lk(g_vfs_mu);
            rc = vfs->readdir(path, ents);
        }
        if (rc == 0) {
            for (auto& d : ents) out.push_back({d.name, d.dtype, d.ino});
        } else {
            err = HostErrno(-rc);
        }
    }
    if (path == "/dev") {
        if (rc != 0) {
            out.push_back({".", lx::dt_dir, proc_ino(path)});
            out.push_back({"..", lx::dt_dir, 1});
        }
        for (const auto& d : kDevNodes) {
            std::string name = d.path + 5;  // after "/dev/"
            bool present = false;
            for (auto& e : out) present = present || e.name == name;
            if (present) continue;
            uint32_t t = d.mode & lx::s_ifmt;
            out.push_back({name, t == lx::s_ifchr ? lx::dt_chr : (t == lx::s_iflnk ? lx::dt_lnk : lx::dt_dir), proc_ino(d.path)});
        }
        return 0;
    }
    if (rc == 0) return 0;
    if (const DevNode* d = dev_node(path)) {
        if ((d->mode & lx::s_ifmt) != lx::s_ifdir) return lx::enotdir;
        out.push_back({".", lx::dt_dir, proc_ino(path)});
        out.push_back({"..", lx::dt_dir, proc_ino("/dev")});
        return 0;  // /dev/shm, /dev/pts: empty until the overlay exists
    }
    return err;
}

// ---- the overlay over the lower tree -----------------------------------------------------

namespace {

class KernelLower final : public LowerFs {
public:
    explicit KernelLower(Kernel& k) : k_(k) {}
    int lstat(const std::string& path, lx::stat& st) override { return lower_stat(k_, path, false, st); }
    int readlink(const std::string& path, std::string& target) override { return lower_readlink(k_, path, target); }
    int readdir(const std::string& path, std::vector<DirEntryInfo>& out) override { return lower_readdir(k_, path, out); }
    std::unique_ptr<FileSource> open(const std::string& path, int& err) override { return lower_open(k_, path, err, nullptr); }

private:
    Kernel& k_;
};

}  // namespace

Overlay& Kernel::overlay() {
    static KernelLower lower(*this);
    static Overlay ov(&lower);
    static const bool booted = [] {
        // Upper-only directories every boot starts with (the rootfs' own
        // /tmp, /run and /var/tmp are hidden: they are empty in the image).
        ov.mkdir_boot("/tmp", 01777, true);
        ov.mkdir_boot("/var/tmp", 01777, true);
        ov.mkdir_boot("/run", 0755, true);
        ov.mkdir_boot("/run/lock", 01777, true);
        ov.mkdir_boot("/run/user", 0755, true);
        ov.mkdir_boot("/run/user/1000", 0700, true);
        ov.mkdir_boot("/dev/shm", 01777, true);
        ov.mkdir_boot("/dev/pts", 0755, true);
        return true;
    }();
    (void)booted;
    return ov;
}

int Kernel::stat_path(const std::string& path, bool follow, lx::stat& st) {
    return overlay().stat(path, follow, st);
}

std::unique_ptr<FileSource> Kernel::open_source(const std::string& path, int& err, lx::stat* st) {
    auto src = overlay().open_source(path, err);
    if (src && st) overlay().stat(path, true, *st);
    return src;
}

int Kernel::readlink_path(const std::string& path, std::string& target) {
    return overlay().readlink(path, target);
}

int Kernel::readdir_path(const std::string& path, std::vector<DirEntryInfo>& out) {
    return overlay().readdir(path, out);
}

}  // namespace rlk

// ---- C API ---------------------------------------------------------------------------------

using namespace rlk;

namespace {

void copy_out(const std::string& s, char* out, size_t cap) {
    if (!out || cap == 0) return;
    if (s.size() < cap) {
        memcpy(out, s.data(), s.size());
        out[s.size()] = 0;
        return;
    }
    // Never hand Swift a truncated (invalid) JSON document.
    snprintf(out, cap, "{\"ok\":false,\"error\":\"reply too large\",\"bytes\":%zu,\"cap\":%zu}", s.size(), cap);
}

}  // namespace

extern "C" void rlk_set_log(rlk_log_fn fn) {
    SetLogSink(fn);
}

extern "C" void rlk_set_vfs(void* rlvfs_vfs) {
    Kernel::get().set_vfs(rlvfs_vfs);
    Log("kernel: guest filesystem %s", rlvfs_vfs ? "attached" : "detached");
}

extern "C" int rlk_exec(const char* const* argv, int argc, const char* const* envp, int envc, const char* cwd, int dry_run,
                        char* out, size_t cap) {
    std::vector<std::string> av, ev;
    for (int i = 0; i < argc; i++) {
        if (argv && argv[i]) av.emplace_back(argv[i]);
    }
    for (int i = 0; i < envc; i++) {
        if (envp && envp[i]) ev.emplace_back(envp[i]);
    }
    if (ev.empty()) {
        ev = {"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/home/user", "USER=user",
              "LOGNAME=user", "LANG=C.UTF-8", "TERM=dumb", "TMPDIR=/tmp"};
    }
    std::string reply;
    int rc = Kernel::get().spawn(av, ev, cwd && *cwd ? cwd : "/", dry_run != 0, reply);
    copy_out(reply, out, cap);
    return rc;
}

extern "C" void rlk_status_json(char* out, size_t cap) {
    copy_out(Kernel::get().status_json(), out, cap);
}

extern "C" size_t rlk_process_output(int pid, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    std::string s = Kernel::get().output_of(pid);
    if (s.size() >= cap) s.erase(0, s.size() - (cap - 1));
    memcpy(out, s.data(), s.size());
    out[s.size()] = 0;
    return s.size();
}

extern "C" int rlk_kill_all(void) {
    return Kernel::get().kill_all();
}

extern "C" int rlk_read_file(const char* path, unsigned char** out, size_t* len) {
    if (!path || !out || !len) return -lx::einval;
    *out = nullptr;
    *len = 0;
    int err = 0;
    auto src = Kernel::get().open_source(NormalizePath(path), err, nullptr);
    if (!src) return -err;
    const uint64_t size = src->size();
    if (size > (256ull << 20)) return -lx::efbig;
    auto* buf = static_cast<unsigned char*>(malloc(size ? (size_t)size : 1));
    if (!buf) return -lx::enomem;
    uint64_t done = 0;
    while (done < size) {
        int64_t n = src->pread(buf + done, (size_t)std::min<uint64_t>(size - done, 1u << 20), done);
        if (n <= 0) break;
        done += (uint64_t)n;
    }
    *out = buf;
    *len = (size_t)done;
    return 0;
}

extern "C" int rlk_write_file(const char* path, const unsigned char* data, size_t len) {
    if (!path || (len && !data)) return -lx::einval;
    Overlay& ov = Kernel::get().overlay();
    Lookup l;
    int e = ov.create(NormalizePath(path), 0644, false, l);
    if (e) return -e;
    std::shared_ptr<TmpNode> node = l.upper;
    if (!node) {
        e = ov.for_write(l.canon, true, node);
        if (e) return -e;
    }
    std::lock_guard<std::mutex> lk(node->data->mu);
    if (!node->data->resize(len)) return -lx::enospc;
    if (len) memcpy(node->data->data(), data, len);
    node->mtime = node->ctime = Overlay::now_sec();
    return 0;
}

extern "C" int rlk_list_dir(const char* path, char* out, size_t cap) {
    if (!path || !out || !cap) return -lx::einval;
    const std::string full = NormalizePath(path);
    std::vector<DirEntryInfo> ents;
    int e = Kernel::get().readdir_path(full, ents);
    if (e) {
        snprintf(out, cap, "{\"ok\":false,\"path\":\"%s\",\"errno\":%d,\"error\":\"%s\"}", JsonEscape(full).c_str(), e, ErrnoName(e));
        return -e;
    }
    std::string s = "{\"ok\":true,\"path\":\"" + JsonEscape(full) + "\",\"entries\":[";
    bool first = true;
    char buf[512];
    for (const auto& d : ents) {
        if (d.name == "." || d.name == "..") continue;
        lx::stat st {};
        const std::string child = full == "/" ? "/" + d.name : full + "/" + d.name;
        const char* type = "other";
        uint64_t size = 0;
        uint32_t mode = 0;
        if (Kernel::get().stat_path(child, false, st) == 0) {
            size = (uint64_t)st.st_size;
            mode = st.st_mode & 07777;
            switch (st.st_mode & lx::s_ifmt) {
                case lx::s_ifdir: type = "dir"; break;
                case lx::s_ifreg: type = "file"; break;
                case lx::s_iflnk: type = "link"; break;
                case lx::s_ifsock: type = "sock"; break;
                case lx::s_ififo: type = "fifo"; break;
                case lx::s_ifchr: type = "chr"; break;
                default: break;
            }
        }
        snprintf(buf, sizeof buf, "%s{\"name\":\"%s\",\"type\":\"%s\",\"size\":%llu,\"mode\":\"%04o\"}", first ? "" : ",",
                 JsonEscape(d.name).c_str(), type, (unsigned long long)size, mode);
        first = false;
        s += buf;
    }
    s += "]}";
    copy_out(s, out, cap);
    return 0;
}
