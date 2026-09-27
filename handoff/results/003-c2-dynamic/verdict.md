# 003-c2-dynamic — build-20 on the Apple TV

**Verdict: PARTIAL.** **C2 PASS**: the glibc hello prints all 7 lines, exit 0. **3a dash**,
**3b busybox**, **3d env** pass, and 3e fails at `pipe2 = ENOSYS` as expected.

**3c `ls -la` is the first step to deviate**, but `ls` itself is fine: it passes when it is the
first guest of a fresh app instance. What fails is **the 4th guest process of any app
instance**. Each finished guest leaves **2 GB of address space reserved**: FEX's rpmalloc heap
(4 × 512 MB spans), never finalized or reused. After three guests, the TV's ~7.25 GB is gone, and
`FEXCore::Allocator::aligned_alloc(8, 24)` returns NULL in
`IntrusivePooledAllocator::ClaimBufferImpl` when the next guest thread starts. The result is a
SIGSEGV at address 0 in host code. It is reproducible: the 4th guest was `ls` in instance 1 and
`hello-dyn` in instance 2.

Build **build-20** (af81c27, IPA sha256 56f489f1…). Instance 1 = app pid 1120 (steps 0–3c),
instance 2 = app pid 1138 (3d, 3e, 4 and the diagnostics). TV time 03:58–04:40 CEST.

## Step 0
- `tv.py install --tag build-20`: ok in 11 s.
- `tv.py launch --fresh` from standby: **the TV woke by itself** (`"turned_on": true`,
  `power_state` was Off). No manual `turn_on` is needed any more.
- `tv.py jit`: ready (prepare 4.39 s, attached/detached tests ok).
- `tv.py vfs mount`: ok (70,376 entries, 626 ms).

## Results
| step | result | files |
|---|---|---|
| 1 **C2** `hello-dyn` | **PASS**, exit 0, pid 1, 43 syscalls, 452 ms; output exactly as expected (`uname: Linux 6.1.0-rltvos #1 SMP PREEMPT_DYNAMIC RLtvOS x86_64`, `pid=1 ppid=0 uid=1000 cwd=/`, `exe=/opt/rl/bin/hello-dyn`, `HOME=/home/user`, `malloc 64MB ok 63`). Reaped: `fds 0`, `mapped_bytes 0`. The ld.so path is in the log (below) | c2.json, c2-stdout.txt, c2-log.txt, c2-shot.png, c2-mem.json |
| 2 `fex status` | `unaligned_fixups: 15` after C2 (37 by the ls fault in instance 1; 55 by the end of instance 2) | fex-status.json |
| 3a `sh -c 'echo shell ok; echo $0 $$'` | **PASS**: `shell ok` / `/usr/bin/sh 2`, exit 0, 49 syscalls, 86 ms | step3a.* |
| 3b `busybox echo busybox ok` | **PASS**: `busybox ok`, exit 0, 48 syscalls, 239 ms | step3b.* |
| 3c `ls -la /opt/rl/bin` (4th guest) | **FAIL**: exit 139, **0 syscalls**, 43 ms: `kernel: pid 4 tid 4 FAULT signal=11 code=2 pc=0x103011dec addr=0x0 (last block-exit guest rip=0x1088a92c0, unaligned fixups so far=37)`. The pc is not in JIT code; see below | step3c.json, step3c-log.txt, fault-3c-symbolized.txt |
| 3c′ same `ls`, **first guest** of instance 2 | **PASS**: exit 0, 204 syscalls, 673 ms, a correct listing (`total 522`, d3d11tri.exe 486115, hello-dyn 16680, hello-static 13632, threads-test 16840; owner `root root`, `.` shows size 0) | diag-ls-first.* |
| 3d `/usr/bin/env` | **PASS**: `PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`, `HOME=/home/user`, `USER=user`, `LOGNAME=user`, `LANG=C.UTF-8`, `TERM=dumb`, `TMPDIR=/tmp`, exit 0, 106 syscalls | step3d.* |
| 3e `sh -c 'echo one \| tr o 0'` | fails as expected: `/usr/bin/sh: 0: Pipe call failed`, exit 2. **First failing call: `[3] pipe2(0x11b3c7b20, 0x0, 0x4, 0x8) = -1 ENOSYS`** (then `close(-1) = -1 EBADF`). The only earlier ENOSYS is the usual `rseq` | step3e.*, log-instance2.txt |
| 4 `ps`, `mem` | instance 2: pids 1–4 all `exited`, **`fds 0`, `mapped_bytes 0`** (reaping works for guest memory). phys_footprint 213.5 MB, peak 214.8 MB, available 1884.5 MB | ps.json, mem-end.json |
| diag: 4th guest in instance 2 = `hello-dyn` | same fault: exit 139, 0 syscalls, `pc=0x102bc9dec addr=0x0` = the same instruction with that instance's slide | diag-4th-hello-dyn.*, log-instance2.txt |

C2's ld.so path, from c2-log.txt:
- `brk(0)`, `access("/etc/ld.so.preload") = -1 ENOENT`, `openat("/etc/ld.so.cache") = 3`,
  `fstat`, `mmap(…, 32187, PROT_READ, MAP_PRIVATE, 3, 0)`, `close`.
- `openat("/lib/x86_64-linux-gnu/libc.so.6") = 3`, `read(3, …, 832)`, `pread64` ×2, `fstat`,
  the 5 libc `mmap`s, `close`.
- `arch_prctl(0x1002, …)`, `set_tid_address = 1`, `set_robust_list`, `rseq = -1 ENOSYS`,
  `mprotect` ×3, `prlimit64`, `munmap`, `getrandom`, `brk` ×2, `uname`, `clock_gettime`,
  `getcwd`, `getuid`, `getppid`, `getpid`.
- `readlink("/proc/self/exe") = 21`, `mmap`/`munmap` 64 MB, one `write(1, …, 211)`,
  `exit_group(0)`.
- Only `rseq` is ENOSYS. The first `read` of libc took 335 ms (VFS fetch).

## The 4th-guest failure: rpmalloc heaps are never released
1. **Where it crashes** (fault-3c-symbolized.txt):
   - I read the app's memory at the fault pc (`rltvos-jit gdb`, `m103011dc0,80`) and found
     those bytes once in build-20's binary, at file offset 0x61dec. So the pc = 0x100061dec +
     slide 0x2fb0000.
   - `llvm-symbolizer` with build-20's dSYM puts it in
     `FEXCore::Utils::IntrusivePooledAllocator::ClaimBufferImpl(unsigned long)`.
   - The disassembly shows `malloc(32)` succeeding, then
     `bl FEXCore::Allocator::aligned_alloc(8, 24)` → **`stp xzr, xzr, [x0]` with x0 = NULL**.
   - The guest never ran a block: rip is still ld.so's entry, and there were 0 syscalls.
2. **Address space shrinks 2 GB per guest process** (`tv.py va --probe2`, instance 2, PROT_NONE
   256 MB steps): fresh **7.25 GB** → after ls **5.25** → after env **3.25** → after the
   pipeline **1.25 GB** (va-*.json). The 64 MB steps give the same picture: 7.44 / 5.44 / 3.44 /
   1.44. Meanwhile phys_footprint only grows 199 → 213 MB, and every process shows
   `mapped_bytes 0` (guest memory is reaped). This is not guest memory.
3. **What holds it** (diag-regions-summary.txt): a `qMemoryRegionInfo` walk of pid 1138 after
   those three guests, done with the debugger. Apart from the dyld shared cache (3.55 GB, normal),
   there are **48 read-write regions of exactly 128 MB (6 GB)**:
   - 8 at 0x121800000–0x161800000 and 40 at 0x7000000000–0x7140000000;
   - XNU cuts large anonymous maps into 128 MB entries;
   - the only other read-write regions are small (4 MB ×5 call-ret stacks, 16 MB ×1, …), so
     FEX's per-thread lookup caches *are* freed.
4. **They are rpmalloc spans** (diag-rpmalloc-span-headers.jsonl). The 256 MB-aligned addresses
   inside them start with rpmalloc `page_t`/`span_t` headers, for example:
   - 0x7000000000: size_class 2, block_size 32, block_count 2044 (a 64 KB page), heap
     0x106748000;
   - 0x130000000: block_size 64, block_count 1022, heap 0x10669c000;
   - 0x7040000000 and 0x7060000000: heap 0x1066a0000.
   That is three different heaps, one per finished guest, each still holding its spans.
5. **Why 2 GB each** (third_party/FEX/…/AllocatorHooks.cpp and rpmalloc.c):
   - rpmalloc 2.0's `SPAN_SIZE` is 256 MB, and spans are mapped with 256 MB alignment.
   - `FEX_rp_mmap` maps `size + alignment` = **512 MB per span** and keeps the slack mapped.
   - A heap maps one span per page type it uses, so 4 spans = 2 GB.
6. **Why never reused:**
   - `InitializeThread()` gives each guest thread (`Kernel::thread_main`) and each HTTP
     connection thread that runs `spawn()` a new rpmalloc heap.
   - Nothing calls `rpmalloc_thread_finalize()` when those threads end, so no heap goes back to
     rpmalloc's queue for the next thread.
   - On Linux, 128 TB of address space hides this; on the TV it's the whole budget.

**LAPTOP's suggestions (REPO decides):**
- (a) Finalize the rpmalloc heap when a guest thread ends, and after `spawn()` on the HTTP
  thread (or do all spawns on one long-lived kernel thread), so the next thread reuses the heap
  and its spans.
- (b) On tvOS, trim `FEX_rp_mmap`'s alignment slack: map, then `munmap` the head and tail, as
  rpmalloc's own OS mapper does. That halves each span. And/or build rpmalloc with a smaller
  `SPAN_SIZE`.
- (c) Consider making `ClaimBufferImpl` treat a NULL allocation as a clean error.

With (a), the process count stops mattering; without it, no program can be the 4th in an app
instance (C3's pipelines and C5's wine chain need dozens).

## Odd things
- **Instance 1 went to the background by itself** (`host: did enter background`), 19 s after
  the ls fault (log 165561.7 ms), and **its debug server never recovered**.
  - LAPTOP sent no key or launch in that window. atvloadly was idle too. The TV then showed the
    home screen (instance1-home-screen.png), possibly from a remote button press.
  - After that, `accept()` failed with `errno=9` (EBADF): tvOS reclaims a suspended app's
    listening socket. `tv.py launch` brought the same instance back (`did become active`),
    but port 7777 stayed dead (connection refused), so `tv.py launch` reported `up: false`
    (instance1-after-background.png shows that log tail).
  - Suggestion: re-create the listener in `applicationDidBecomeActive`, or when `accept` returns
    EBADF.
  - No crash happened, so there is no crash-N.txt.
- **LAPTOP's own mishap:** my first memory-map walk of instance 1 crawled through the JIT pool
  (8,192 separately blessed 16 KB pages). My 400 s timeout killed the helper while it was still
  attached, which left the app stopped. I killed it with SIGKILL through CoreDevice's appservice
  (new `rltvos-jit signal`) and started instance 2 with `tv.py launch --fresh`. The second walk
  started above the pool and took 12 s.
- strace formatting: unimplemented calls print 4 raw arguments (`pipe2(0x11b3c7b20, 0x0, 0x4,
  0x8)`; pipe2 has 2). `prlimit64` is printed as `prlimit_64`. Control characters are escaped now
  (`\n`).
- The address-space budget of a fresh build-20 instance is 7.25 GB (build-18's probe after JIT
  was 6.25 GB).

## Files
verdict.md; c2.json, c2-stdout.txt, c2-log.txt, c2-shot.png, c2-mem.json; fex-status.json;
step3a–3e `.json`/`-stdout.txt`; step3c-log.txt; ps.json, mem-end.json; diagnostics:
diag-ls-first.*, diag-4th-hello-dyn.*, va-0-fresh / 1-after-ls / 2-after-env / 3-after-pipe.json,
diag-regions-summary.txt, diag-rpmalloc-span-headers.jsonl, fault-3c-symbolized.txt,
log-instance2.txt, end-shot.png, instance1-home-screen.png, instance1-after-background.png;
launch.json, relaunch-1.json (`up: false`), relaunch-2.json, jit.json, jit-2.json,
vfs-mount*.json.
