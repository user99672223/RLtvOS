# Request 003 — C2 (glibc hello) on the TV, plus first dynamic programs

**Build:** `build-20` (release body `FEX built: true, FEX linked: true`; a later
green build with the same line is fine — note the tag).

What changed since 002: the fault guard now handles the SIGBUS you decoded
(FEX's TSO `ldapur` on an access crossing 16 bytes) exactly as FEX's Linux
frontend does — `HandleUnalignedAccess(HalfBarrier)` back-patches the
instruction through the pool's RW alias and the thread resumes — so ld.so's
`memcmp` should just continue. Exited guest processes are reaped now (memory
and fds freed; `ps` keeps the entry), the strace log escapes control
characters, and `tv.py launch` runs `atvremote power_state`/`turn_on` first.
Thanks for the decoded JIT block; that made this a one-line diagnosis.

No rootfs or manifest change is needed (same binaries as 002).

## Steps (save every JSON reply; `tv.py exec` prints the guest output before its JSON)

0. `tv.py install --tag build-20`, `tv.py launch --fresh` (wakes the TV itself
   now — tell me if it still needs a manual `turn_on`), `tv.py jit` → ready,
   `tv.py vfs mount`.
1. **C2** — `tv.py exec -- /opt/rl/bin/hello-dyn`. Expected (exit 0; pid, the
   monotonic value and HOME's default are free):
   ```
   hello from x86-64 glibc (argc=1)
   uname: Linux 6.1.0-rltvos #1 SMP PREEMPT_DYNAMIC RLtvOS x86_64
   monotonic: <s>.<ns>
   pid=1 ppid=0 uid=1000 cwd=/
   exe=/opt/rl/bin/hello-dyn
   HOME=/home/user
   malloc 64MB ok 63
   ```
   Save `c2.json`, `c2-stdout.txt`, `c2-log.txt` (`tv.py log --all --out …`:
   the whole ld.so path — openat ld.so.cache, libc.so.6, mmap/mprotect,
   arch_prctl, brk, set_tid_address, rseq = ENOSYS …), `c2-shot.png`,
   `c2-mem.json`.
2. `tv.py fex status` → `fex-status.json`: `status.unaligned_fixups` is the
   number of back-patched instructions so far (expect a handful after C2).
3. First real programs (each its own `tv.py exec`, save `stepN.json` +
   `stepN-stdout.txt`; on the first failure also `tv.py log --all`):
   - `tv.py exec -- /usr/bin/sh -c 'echo shell ok; echo $0 $$'` → `shell ok` /
     `/usr/bin/sh <pid>`, exit 0 (dash, no fork).
   - `tv.py exec -- /bin/busybox echo busybox ok` → `busybox ok`, exit 0.
   - `tv.py exec -- /bin/ls -la /opt/rl/bin` → a listing of hello-static,
     hello-dyn, threads-test (and d3d11tri.exe if built), exit 0. This walks
     getdents64, newfstatat/statx fallback, ioctl TIOCGWINSZ, and locale
     files under /usr/lib/locale (expect some ENOENT lines — fine).
   - `tv.py exec -- /usr/bin/env` → the kernel's default environment (PATH,
     HOME, USER, LOGNAME, LANG, TERM, TMPDIR).
   - `tv.py exec -- /usr/bin/sh -c 'echo one | tr o 0'` → expected to FAIL
     (no pipe2/vfork yet): report the first `= -1 ENOSYS`/`EAGAIN` line —
     that is the C3 starting point.
4. `tv.py ps` → `ps.json` (all entries should show `mapped_bytes: 0` and
   `fds: 0` after exit: reaping), `tv.py mem` → `mem-end.json`.
5. If a guest faults: the `kernel: pid N tid M FAULT signal=… code=… pc=…
   (in JIT code) addr=… (last block-exit guest rip=…, unaligned fixups so
   far=…)` line plus the ~40 log lines before it; the app should survive. The
   same `rltvos-jit peek` decoding you did for 002 is welcome again if the
   fault is in JIT code.

## Pass criteria
- **PASS**: step 1 prints its lines with exit 0 and steps 3a–3d exit 0 with
  the expected output (3e failing is expected).
- **PARTIAL**: name the first step that deviates and quote the last ~20
  strace lines before it.
- **FAIL**: app crash → `crash-N.txt` + the log tail.

## Files: `handoff/results/003-c2-dynamic/`
`verdict.md`, `c2.json`, `c2-stdout.txt`, `c2-log.txt`, `c2-shot.png`,
`c2-mem.json`, `fex-status.json`, `step3a…3e.json/-stdout.txt`, `ps.json`,
`mem-end.json`, `crash-*.txt` if any.
