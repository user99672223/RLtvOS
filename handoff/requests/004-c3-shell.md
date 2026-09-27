# Request 004 — C3 on the TV: busybox sh pipeline with a background job, threads-test; VA budget across guests

**Build:** `build-23` (release body `FEX built: true, FEX linked: true`; a later
green build with the same line is fine — note the tag).

Result 003's 4th-guest death: FEXCore now uses the system allocator on Darwin
(no rpmalloc thread heaps; DECISIONS 2026-09-27). `tv.py fex` / the init JSON
shows `"allocator":"system"` in this build. The debug server also re-creates
its listener when tvOS reclaims it after a background trip.

New in this build (kernel C3): fork/vfork/clone, execve (+ `#!` scripts),
wait4/SIGCHLD, pipe2, a real futex, guest signal delivery (rt_sigframe,
rt_sigreturn, sigaltstack, kill/tgkill, rt_sigsuspend/pause, SA_RESTART),
interruptible sleeps. fork runs vfork-style behind a copy-on-write snapshot
(DECISIONS 2026-09-27), so a shell child that runs builtins does not corrupt
its parent; a background job therefore *blocks the parent until the child
execs or exits* — the script's output order can differ from Linux, the content
must not. Signals reach a thread only at its next syscall (a thread spinning
in JIT code notices later); `threads-test` sends its SIGUSR1 to itself, which
is delivered at that syscall's return.

## Prep (laptop)

1. `git pull`. Put the checkpoint scripts into the rootfs so the TV can run
   them: `mkdir -p $ROOTFS_DIR/opt/rl/refs && cp laptop/refs/guest/*.sh
   $ROOTFS_DIR/opt/rl/refs/ && chmod +x $ROOTFS_DIR/opt/rl/refs/*.sh`
   (10-rootfs.sh does this on its next run). Rebuild the manifest
   (`--manifest-only --rebuild`), restart `rltvos-assets`.
2. `tv.py install --tag build-23`, `tv.py launch --fresh`, `tv.py jit`,
   `tv.py vfs mount`. `tv.py fex init` → `fex-init.json` (the reply must
   contain `"allocator":"system"`).

## Steps (save every JSON reply and the guest output; `tv.py exec` waits and prints both)

0. **VA budget across guests** (the result-003 regression): `tv.py va --probe2` →
   `va-0-fresh.json`; then `tv.py exec -- /usr/bin/env` four times; `tv.py va
   --probe2` → `va-1-after-4.json`; run `/usr/bin/env` twice more (guests 5 and
   6) → `step0-env5.json`, `step0-env6.json` (must still print the environment,
   exit 0); `tv.py va --probe2` → `va-2-after-6.json`. Expect `none_256mb.gb`
   within 0.5 GB of the fresh value throughout (build-20 lost 2 GB per guest).
1. Pipes and fork, smallest first:
   - `tv.py exec -- /usr/bin/sh -c 'echo one | tr o 0'` → `0ne`, exit 0 → `step1a`.
   - `tv.py exec -- /usr/bin/sh -c 'x=$(echo hi); echo got:$x; (echo sub; exit 3); echo rc=$?'`
     → `got:hi`, `sub`, `rc=3` → `step1b`.
   - `tv.py exec -- /usr/bin/sh -c 'sleep 1 & echo bg=$!; wait; echo waited'` →
     `bg=<pid>`, `waited` (about 1 s) → `step1c`.
2. **C3 proper**: `tv.py exec -- /opt/rl/refs/c3.sh` (a `#!/bin/sh` script that
   execs `/bin/busybox sh -c '…'`). Expected output, in this order or with the
   background line earlier (see above):
   ```
   C3 start pid=<pid>
   3
   c
   b
   got-TERM
   after self-kill
   background job done
   false exit status: 1
   Name:	sh
   Umask:	0022
   State:	R (running)
   threads: counter=400000 (expect 400000) joined=60 (expect 60) usr1=1 tid=<pid>
   threads-test exit=0
   C3 done
   ```
   exit 0. Save `c3.json`, `c3-stdout.txt`, `tv.py log --all --out c3-log.txt`
   (this is the trace to diff against `refs/c3.summary.txt`: clone/execve/
   pipe2/dup2/wait4/kill/rt_sigaction/rt_sigreturn/futex lines),
   `tv.py shot --out c3-shot.png`, `tv.py mem` → `c3-mem.json`.
3. `tv.py exec -- /opt/rl/bin/threads-test` alone → the `threads:` line, exit 0
   → `threads.json` (+ log if it fails: futex/clone lines).
4. `tv.py exec -- /usr/bin/sh -c 'kill -9 $$'` → no output, exit 137 (signal 9) and
   `tv.py exec -- /usr/bin/sh -c 'trap "echo caught; exit 5" TERM; kill -TERM $$; echo not-here'`
   → `caught`, exit 5 → `step4a/b`.
5. `tv.py ps` → `ps.json` (every finished process `state exited`, `mapped_bytes 0`,
   children carry the shell's pid as `ppid`), `tv.py mem` → `mem-end.json`.
6. On a hang (`tv.py exec` waits up to 120 s): `tv.py ps`, `tv.py log --all
   --out hang-log.txt`, then `tv.py ps --killall`; report which process/thread
   is stuck and its last strace line. On a FAULT line: the ~40 lines before it.

## Pass criteria
- **PASS**: step 0 holds the VA budget (6 guests run, `none_256mb.gb` within
  0.5 GB of fresh) and steps 1–4 produce the expected lines with the expected
  exit codes with the app alive afterwards (order of the background line is
  free).
- **PARTIAL**: name the first deviating step and quote the last ~20 strace
  lines before it (the next task on my side). If step 0 still loses address
  space, stop there and add `laptop/jit regions` for the instance.
- **FAIL**: app crash → `crash-N.txt` + the log tail.

## Files: `handoff/results/004-c3-shell/`
`verdict.md`, `fex-init.json`, `va-0-fresh.json`, `va-1-after-4.json`,
`va-2-after-6.json`, `step0-env5/6.json`, `step1a…1c.json/-stdout.txt`,
`c3.json`, `c3-stdout.txt`, `c3-log.txt`, `c3-shot.png`, `c3-mem.json`,
`threads.json`, `step4a/b.json`, `ps.json`, `mem-end.json`,
`hang-log.txt`/`crash-*.txt` if any.
