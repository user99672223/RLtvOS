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
`epoll_*` (as built, C4): every guest fd is a kernel object (pipe, unix
socket, eventfd, epoll instance, file), none is a host fd, so polling is
our own: `OpenFile::poll(PollTable*)` returns the readiness bits and
registers the caller's `Waiter` on the wait queues that change them
(`poll.h`); `DoPoll` registers, checks, sleeps, repeats — the Linux
poll_table pattern. `epoll` is an interest list scanned the same way
(level-triggered; `EPOLLET` reports on the rising edge only;
`EPOLLONESHOT` disarms). A signal interrupts any of them with `EINTR`
(never restarted after a handler, like Linux; restarted transparently
when the signal is ignored). Interval timers: one kernel thread posts
`SIGALRM` at the deadline (`timers.cpp`); it is delivered at the next
syscall boundary like every guest signal.

## 4. Files and namespace

`OpenFile` kinds (as built): `RegularFile` (read-only lower file: rlvfs
through the block cache, or a generated /proc text), `TmpFile` (a file of
the upper layer: read/write/pwrite/ftruncate, `O_APPEND`), `DirFile`
(snapshot listing of the merged directory), `PipeFile` (pipes and FIFOs),
`SocketFile` (AF_UNIX), `EventFdFile`, `EpollFile`, `ConsoleFile`
(stdio → log + capture), `DevNullFile` (null/zero/full/urandom),
`PathFile` (`O_PATH`).

Namespace (`overlay.h`): one tree. The **lower** layer is read-only:
rlvfs (rootfs + prefix + game from the laptop), the synthetic `/proc`, and
the `/dev` fallback nodes. The **upper** layer is an in-memory tmpfs
(`TmpNode`s) consulted first at every component: a name found there wins
(or is a *whiteout* → ENOENT), otherwise the lookup falls through to the
lower tree at the same path; directory listings merge both minus
whiteouts; symlinks of either layer are followed component by component,
so `/var/run → /run` lands in the upper `/run`. Every mutation happens in
the upper layer: `O_CREAT`, `mkdir`, `symlink`, `mknod` (FIFO, socket)
create upper nodes (the parent chain is materialized from the lower
directory's metadata); writing a lower file (`O_WRONLY|O_RDWR|O_TRUNC`,
`truncate`, `link`, `rename`, `chmod`) copies it up first; `unlink`/`rmdir`
of a lower name leaves a whiteout; a `mkdir` where a lower directory was
removed is *opaque* (the lower contents stay hidden). `rename` of a
directory that is merged with a lower one → EXDEV (like overlayfs without
redirects). `/tmp`, `/var/tmp`, `/run` (+`lock`, `user/1000`), `/dev/shm`,
`/dev/pts` are opaque upper directories created at boot. Nothing persists:
the upper layer lives as long as the app (Caches are purgeable anyway;
persistence is a later decision, DECISIONS 2026-09-27). `memfd_create` is
an upper file without a name. `MAP_SHARED` of an upper file: the file's
storage moves into a `SharedStore` (Linux: a memfd; Darwin: an anonymous
mapping aliased with `vm_remap`) and the guest range becomes a host alias of
the store when the mapping is host-page aligned and exclusively the
process's own (Wine's 64 KB views are) — server and clients then see one
another's writes, `read`/`write` on the file included. A misaligned mapping
falls back to a private copy written back on `munmap`/`msync`/exit/execve
(`SharedMap`). Fork snapshots skip aliased pages; `MAP_FIXED` over an alias
replaces the page with private memory instead of zeroing the file.

Inodes: rlvfs = hash of the canonical path (`st_dev` 0x801); upper nodes =
a counter from 0x200000 on `st_dev` 0x14 (so a copied-up file changes
inode, like overlayfs); pipes/sockets/anon inodes have their own ranges.

Record locks (`locks.h`): all guest processes share one host process, so
POSIX locks (`F_SETLK/F_SETLKW/F_GETLK`, owned by the pid, dropped when it
closes *any* descriptor of the file), OFD locks (`F_OFD_*`, owned by the
description) and `flock` (per description, whole file) are one table keyed
by `(st_dev, st_ino)` with the real conflict/split/merge semantics;
`F_SETLKW` sleeps on a wait queue woken by every unlock. wineserver's
`lock` file and Wine's `F_GETLK` probe of it (C5) need exactly this.

Unix sockets (`socket.h`): in-process objects, not host sockets. Stream,
dgram and seqpacket; `socketpair`; names in the filesystem (an upper Sock
node holding a weak reference to the listener; `connect` to a name whose
socket is gone → ECONNREFUSED, like Linux) and in the abstract namespace
(libxcb tries `@/tmp/.X11-unix/X0` first, then the path). A connection is
two `UnixSocket`s pointing at each other, each with a receive queue of
messages carrying bytes, `SCM_RIGHTS` files (installed into the receiver's
fd table by `recvmsg`, one set per call, `MSG_CMSG_CLOEXEC`) and the
sender's credentials (`SO_PASSCRED` → `SCM_CREDENTIALS`, `SO_PEERCRED` =
the peer process's pid/1000/1000). `rcvbuf` flow control, `SHUT_*`,
`POLLHUP|POLLRDHUP` on peer close, `EPIPE` + `SIGPIPE` unless
`MSG_NOSIGNAL`. `AF_INET`/`AF_NETLINK` → EAFNOSUPPORT (no network: Xvfb
runs `-nolisten tcp`; glibc's `getifaddrs` tolerates the netlink failure).

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

## 3. Processes, threads and signals (C3)

- **Threads** (`clone` with CLONE_THREAD): a new `GuestThread` + FEXCore thread
  in the same process with the given stack and TLS; CLONE_*_SETTID /
  CHILD_CLEARTID honoured; thread exit clears the tid word and wakes its futex.
- **fork** (`clone(SIGCHLD)`, `fork`): vfork-style — the child runs in the
  parent's `AddressSpace` (shared `shared_ptr`) on its own host thread while
  the parent blocks; `AddressSpace::push_snapshot()` write-protects every
  writable host page and records the VMA table and brk. The child's first write
  to a tracked page faults into `rlfex`'s guard → `Kernel::host_fault_hook`
  → `cow_fault()`, which saves the old 16 KB and re-enables writing; kernel
  writes into guest memory (`copy_in`, `zero`, `unmap`, `MAP_FIXED`) save the
  page first. The guard classifies each fault from the arm64 exception
  syndrome (`uc_mcontext->__es.__esr`: fault status 0x21 alignment, 0x0D–0x0F
  permission, 0x04–0x0B translation) because XNU reports every SIGBUS with
  si_code 1, alignment and write-protection faults alike (result 005); only
  permission/unknown faults reach the hook, alignment faults go straight to
  FEX's back-patcher, and `cow_fault()` never claims the same already-saved
  page twice in a row (result 004: a claimed alignment fault looped forever).
  The guest signal follows the classification (SIGSEGV for permission and
  translation faults, SIGBUS for alignment), not Darwin's signal number. When the child execs or exits, `pop_snapshot()` unmaps what the
  child mapped, restores the VMA table, brk and every saved page, drops the
  translations of those pages and the parent continues. Snapshots nest.
  **vfork** (CLONE_VM|CLONE_VFORK) does the same without a snapshot.
- **execve**: loads the new image into a fresh `AddressSpace` (shebang
  handled), swaps it in, closes CLOEXEC fds, resets handlers to SIG_DFL and
  the calling thread's CPU state (registers, flags, FPU, callret stack) in
  place; FEX's dispatcher then looks up the new RIP. The old space is dropped
  (unmapped + translations invalidated) unless a forked child still shares it.
- **Blocking**: every guest thread owns a `Waiter`; pipes, futex words, child
  exit and timers keep it in `WaitQueue`s (prepare/notify generation counting,
  no lost wakeups). A signal notifies the waiter → the syscall returns -EINTR
  and is restarted after the handler when the action has SA_RESTART (or
  transparently when the signal was dropped).
- **Signals**: pending bits per thread and per process, ignored signals
  dropped at send time; delivery at the end of a syscall builds the x86-64
  `rt_sigframe` (ucontext with the CPUState registers, EFLAGS via
  `ReconstructCompactedEFLAGS`, 512-byte fxsave area, siginfo) on the guest
  stack or the sigaltstack and enters the handler; `rt_sigreturn` restores
  everything. Default actions: terminate (recorded for wait4) or ignore.
  Asynchronous delivery into JIT code is not implemented yet (see DECISIONS).
- **wait4**: children are found through `ppid` in the process table; zombies
  keep their entry until waited; a child whose parent is gone is reaped at
  exit. `SIGCHLD` with CLD_EXITED/CLD_KILLED siginfo goes to the parent.
