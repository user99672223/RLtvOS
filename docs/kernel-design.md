# Fake kernel design (Phase C) — working notes

Status: draft, written before FEXCore is vendored. FEX-specific hooks are
marked `FEX:` and get filled in from the FEX mapping (see NEXT.md). Numbers
marked `MEASURE` come from request 001 (`/va`, `/mem`, `sigaltstack`).

## 1. Process model

One tvOS process hosts everything. A *guest process* is a thread group plus
per-process state:

| Field | Notes |
|-------|-------|
| `pid`, `ppid`, `pgid`, `sid` | small integers from 2 upward (pid 1 = init, our script runner) |
| `tid`s | one host pthread per guest thread; guest tid = unique small int; `gettid` returns it; host thread ↔ guest thread map for tgkill/pthread_kill |
| VA slice | reserved range for this process's mappings (see §2); `brk` base/current inside the slice |
| fd table | `vector<FdEntry>` with FD_CLOEXEC flag; `FdEntry` = refcounted `OpenFile` (file/dir/pipe/socket/eventfd/epoll/memfd/...) + flags |
| cwd, root, umask | strings/ints |
| signal state | per-process `sigaction[65]`, per-thread mask + pending set, alt stack per thread |
| exit state | exit code, zombie until `wait4` by parent; SIGCHLD to parent |
| credentials | uid=gid=1000 (`user`), euid same; `getuid` etc. constant; root not emulated |
| identity | comm (from execve path), argv/envp copy for `/proc/<pid>/cmdline` |

Threads: `clone(CLONE_VM|CLONE_THREAD|...)` → `pthread_create` of a host
thread that runs a new FEX thread object on the same guest context.
`CLONE_SETTLS` → guest FS base; `CLONE_CHILD_CLEARTID`/`set_tid_address` →
on thread exit write 0 and `futex_wake` the address. `clone3` → ENOSYS
(glibc falls back to `clone`).

## 2. Memory

Measured on the TV (request 001, build-11): `/va` reports **max contiguous
6 GB and only 7 × 1 GB PROT_NONE reservations in total** (ENOMEM after
that) although the address range spans 0x1074_00000–0x71_8000_0000.
`vaprobe2` (build-14) separates granularity / protection / API effects;
until it says otherwise the budget for *all* guest processes together is
**≈ 6.5 GB of virtual address space** (vaprobe2, result 002: 6.25 GB in 256 MB steps, 6.5 GB in 64 MB steps, the same for PROT_NONE, RW, RW+NORESERVE and vm_allocate; RLIMIT_AS unlimited), code buffers and JIT pool included.

Consequences (replaces the per-process slice plan):

- **One shared guest address space** (guest address = host address; no
  per-process VA slices). A guest process owns a list of VMAs (start, len,
  prot, kind, file, offset) used for `/proc/<pid>/maps`, `munmap` on exit
  and bookkeeping; the memory itself is allocated lazily from the host with
  plain `mmap`. Two guest processes cannot both map the same fixed address
  — the second `MAP_FIXED` request into a range another process owns fails
  with EEXIST-like ENOMEM and is logged (`vma-conflict`). Wine tolerates
  relocation of PE images.
- **The low 4 GB does not exist for the guest.** XNU refuses to exec an
  arm64 64-bit binary whose `__PAGEZERO` is smaller than 4 GB
  (`bsd/kern/mach_loader.c`: "64 bit ARM binary must have hard page zero of
  4GB", `LOAD_BADMACHO`), so `-pagezero_size` cannot open it and no
  `mmap` below 0x100000000 ever succeeds (`/va` on the TV starts at
  0x107400000). Consequences: every guest executable must be PIE
  (`AddressSpace::min_addr()` = 4 GB; the loader refuses ET_EXEC images
  below it with ENOEXEC and "relink as PIE"; `hello-static` is built
  `-static-pie`, `busybox` replaces `busybox-static`;
  `laptop/setup/45-elf-audit.sh` lists offenders). Wine's one fixed low page,
  KUSER_SHARED_DATA at 0x7ffe0000, is a C5 problem: plan A keeps Wine stock
  and redirects the JIT's loads/stores that fault on that page to a relocated
  copy (FEX already decodes host load/store instructions for its unaligned-
  access handler); plan B is a one-constant Wine rebuild on the laptop.
- ET_DYN binaries (main PIE, interpreter, libraries) are loaded wherever the
  host has room; the main image and the interpreter each get one reservation
  for their whole span so their segments keep their relative layout.
- 4 KB guest pages on 16 KB host pages: a shadow protection table with one
  byte per 4 KB page for every VMA; the host page gets the union of the four
  4 KB protections. Guest code is never executed by the host (FEX reads it),
  so host protections never include EXEC; a guest `PROT_NONE` sub-page is
  only enforced at 16 KB granularity (Wine's guard pages: acceptable).
- `brk`: 64 MB reserved after the executable, committed on demand.
- File mappings: eager copy from the VFS into anonymous memory for the
  mapped range (MAP_PRIVATE semantics for free); large read-only mappings
  get a fault-driven fill later only if `/mem` says the eager copies hurt.
- `munmap` returns memory to the host (`munmap`) — reservations are not
  kept because VA is the scarce resource here.
- `vfork` model: the child shares the parent's VMAs until `execve` or
  `_exit` (it is the same address space anyway); on `execve` the child gets
  fresh VMAs, the parent resumes. Anything a vfork child does besides
  dup2/close/sigprocmask/chdir/setsid/execve/_exit is logged `vfork-unsafe`.
- FEX code buffers come out of the 128 MB TXM pool (`jit26.c`); FEX's
  `MAX_CODE_SIZE` is 64 MB on Apple so growth never needs 192 MB live.

## 3. Scheduling and blocking

Host pthreads scheduled by XNU; no guest scheduler. Blocking syscalls block
the host thread. `futex`: hash table of wait queues keyed by address;
`FUTEX_WAIT` = `os_sync_wait_on_address` on the guest address itself
(32-bit value compare + wait; fallback `__ulock_wait`); `FUTEX_WAKE` =
`os_sync_wake_by_address_any/all`; `FUTEX_REQUEUE`/`CMP_REQUEUE`
(pthread cond vars in glibc use `FUTEX_WAKE`/`WAIT` mostly; requeue used by
`pthread_cond_broadcast`) implemented with our own wait-queue table when
the host primitive cannot requeue: plan A is our own queue table (mutex +
condvar per bucket) for everything, using the host primitive only as an
optimisation if profiling asks for it. `FUTEX_PI` → ENOSYS unless Wine
needs it (`MEASURE` from the C5 trace). Timeouts: absolute/relative per
flags, `CLOCK_MONOTONIC`/`REALTIME`.

`nanosleep`/`clock_nanosleep`, `poll`/`ppoll`/`select`/`pselect6`,
`epoll_*`: implemented over `kqueue`/`poll` on host fds for file-like
objects and over our own wait queues for guest-only objects (eventfd,
pipes between guests are host pipes — fine; unix sockets are host unix
sockets in a private directory — fine; so nearly everything is a host fd
and `poll(2)` on the host works). `epoll` → per-instance `kqueue`.

## 4. Files and namespace

`OpenFile` kinds: `VfsFile` (read-only manifest file through the block
cache; `pread` served from the cache), `OverlayFile` (host fd in the
overlay dir), `HostFd` (pipes, sockets, eventfd emulation via pipe or
kqueue EVFILT_USER, `/dev/null` etc.), `Synthetic` (procfs/sysfs/devfs text
generated on open, seekable), `Dir` (readdir cursor over merged
VFS + overlay + synthetic entries).

Path resolution: guest absolute path → mount table:
`/` rootfs (RO), `/prefix` prefix (RO base + RW overlay), `/game` (RO),
`/home/user` (RO base + RW overlay), `/tmp` `/run` `/var/tmp` (RW overlay
only, wiped at boot), `/proc` `/sys` `/dev` synthetic, `/dev/shm` RW overlay.
Overlay: `Caches/overlay/<mount>/<path>`; whiteouts as `.wh.<name>` files;
copy-up on first write (`O_WRONLY|O_RDWR|O_TRUNC|O_APPEND`, `truncate`,
`chmod`, `rename`, `unlink`). Directory listings merge base + overlay
minus whiteouts. `rename` across mounts → EXDEV. `fcntl` locks: per guest
process lock table keyed by (inode id, range) with POSIX semantics
(Xvfb's `/tmp/.X0-lock` uses `O_EXCL` + `link`, wineserver uses `fcntl`
locks on the config dir and `flock`-like `F_SETLK`).

Inodes: 64-bit hash of the canonical manifest path (stable across runs) for
base files; overlay files use the host inode | 1<<60; synthetic ids are
small constants.

`/proc`: `self` → pid; `<pid>/{maps,exe,status,stat,cmdline,cwd,fd/,
fdinfo/,mem,auxv,comm,task/}`, `cpuinfo`, `meminfo`, `stat`, `uptime`,
`loadavg`, `sys/kernel/{pid_max,osrelease,ostype,version,random/*}`,
`sys/vm/*`, `filesystems`, `mounts`, `version`. `/proc/<pid>/mem`:
`pread`/`pwrite` are memcpy within the same host task (Wine uses this for
`ReadProcessMemory` between guest processes). `/sys`: `devices/system/cpu`
(online, possible, cpuN/topology, cache), `class/input` (evdev), `bus/pci`
empty, `fs/cgroup` empty. `/dev`: null, zero, full, random, urandom, tty,
pts/, ptmx (Wine wants a pty only for conhost; return ENODEV until needed),
shm/ (overlay dir), input/eventN (synthetic, §6), fd → /proc/self/fd,
dri (absent → Vulkan goes through the thunk anyway).

## 5. Signals

Per-thread pending set and mask; per-process handlers. Sources:
`kill`/`tgkill`/`rt_sigqueueinfo` from guests, synchronous faults from
FEX's fault delegator (SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGTRAP translated with
`si_addr`/`si_code`), timers (`setitimer`/`timer_create` → host dispatch
timer that injects SIGALRM), SIGCHLD on child exit, SIGPIPE on broken pipe
(host SIGPIPE ignored; we detect EPIPE and raise to the guest).

Delivery to a running JIT thread: `FEX:` deferred-signal path — set the
target thread's pending bit and poke it (`pthread_kill(host, SIGUSR1)`
whose host handler only marks the FEX thread to leave the JIT at the next
safe point; the guest frame is then built by FEX's signal frame code with
the x86-64 `rt_sigframe` layout and `rt_sigreturn` restores it). Blocked
threads (in a syscall) are woken with EINTR semantics (`SA_RESTART`
honoured for the restartable set).

`sigaltstack`: guest alt stacks are just guest memory; the host alt stack
question (`MEASURE`: `/status.sysinfo.sigaltstack`) only affects our own
fault handler: without it, host SIGSEGV handlers run on the faulting
thread's stack, which is the *guest* stack while in JIT code — FEX's JIT
keeps the host SP on a host stack? `FEX:` verify; if the JIT runs on the
guest stack, a guest stack overflow kills us silently. Mitigation if
`sigaltstack` fails: run JIT'd code on a dedicated host stack per thread
(FEX Windows layer already does this to handle stack switches) — check.

## 6. Devices for later phases

- `/dev/input/eventN`: synthetic evdev device fed from
  `GCController` events; `EVIOCGBIT/EVIOCGABS/EVIOCGNAME/EVIOCGID` ioctls
  answered from a static Xbox-style descriptor; reads return `input_event`
  records; `poll` support via an eventfd-like host pipe.
- Vulkan: thunk over the guest `libvulkan.so.1` — separate design doc.
- Audio: none (`winealsa`/`winepulse` absent; Wine picks no driver).

## 7. Guest log (strace format)

Every syscall entry/exit is formatted like `strace -f -tt -y`:
`pid  HH:MM:SS.uuuuuu name(args) = ret [ERRNO (text)]` into `rl_log` with
a `guest:` prefix stripped by `tv.py log --strace`. Rate limits: a ring of
8192 lines is small; `/log` streaming via `tv.py log --follow` keeps up at
~2000 lines/s over LAN. Filters: `RL_STRACE=all|errors|none` per process
via the init script's environment.

## 8. Boot sequence (init = pid 1)

`POST /run {"argv":["/bin/sh","/refs/guest/c5.sh"],...}` → kernel creates
pid 1 with the given argv/env/cwd, execve → ELF loader (FEX's) → JIT
thread. Exit of pid 1 ends the "boot"; other processes are killed. Later
phases drive `init` with the checkpoint scripts unchanged from the laptop.

## 9. Checkpoint → syscall coverage

| Checkpoint | New surface (from the laptop traces, refined per run) |
|------------|-------------------------------------------------------|
| C1 | `write`, `exit_group` |
| C2 | `openat`, `read`, `pread64`, `close`, `fstat`/`newfstatat`, `mmap`, `mprotect`, `munmap`, `brk`, `arch_prctl`, `set_tid_address`, `set_robust_list`, `rseq`(ENOSYS), `prlimit64`, `getrandom`, `readlink`, `uname`, `clock_gettime` (vDSO absent → real syscall), `getpid`/`getppid`/`getuid`..., `getcwd`, `exit_group`, `writev`, `ioctl(TCGETS)` (ENOTTY) |
| C3 | `clone`/`vfork`, `execve`, `wait4`, `pipe2`, `dup2`/`dup3`, `fcntl`, `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`, `kill`, `tgkill`, `nanosleep`, `futex`, `sigaltstack`, `getpgrp`, `setpgid`, `lseek`, `fchdir`, `ioctl(TIOCGPGRP)` |
| C4 | `socket`/`bind`/`listen`/`accept4`/`connect` (AF_UNIX), `getsockopt(SO_PEERCRED)`, `poll`/`select`/`epoll_*`, `shutdown`, `sendmsg`/`recvmsg` (SCM_RIGHTS for MIT-SHM disabled → not needed), `link`/`unlink`/`mkdir`/`umask`/`chmod`, `getdents64`, `statfs`, `flock`/`fcntl(F_SETLK)`, `setsockopt`, `eventfd2`, `timerfd_*` (Xvfb) |
| C5 | + `memfd_create`, `ftruncate`, `mremap`?, `madvise`, `sched_*`, `prctl(PR_SET_NAME)`, `getrlimit`/`setrlimit`, `sysinfo`, `/proc/self/*`, `/proc/<pid>/mem`, `ptrace` (only if traced), `socketpair`, `recvmsg(SCM_RIGHTS)` (wineserver passes fds!), `epoll_pwait`, `timer_create`, `inotify_*` (ENOSYS ok), `getxattr` (ENOTSUP), `statx`, `faccessat2`, `copy_file_range`, `sendfile`, `clock_nanosleep`, `sched_yield`, `membarrier` (ENOSYS), `mincore`, `msync` |

Each request from C1 on: run the laptop reference trace summary
(`laptop/refs/summarize.py --diff ref.trace.gz tv-log.txt`) and fix every
ENOSYS/wrong return before the next request.
