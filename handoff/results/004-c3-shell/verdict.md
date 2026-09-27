# 004-c3-shell — build-23 on the Apple TV

**Verdict: PARTIAL (steps 2–5 not run yet).**
- **Step 0 PASS:** the address-space budget holds (7.5 → 7.25 → 7.25 GB across 6 guests;
  `allocator: system`).
- **Step 1b PASS:** command substitution, subshell and `$?`.
- **Steps 1a (`echo one | tr o 0`) and 1c (`sleep 1 & …; wait`) HANG.** The forked child
  (vfork-style, running on the parent's copy-on-write snapshot) gets stuck in an endless fault
  loop. An unaligned TSO store (`stlurh`) into a snapshotted heap page raises an alignment
  SIGBUS. `host_fault_hook` → `AddressSpace::cow_fault()` claims it ("resolved, retry") every
  time, so FEX's unaligned fix-up is never reached.

Steps 2–5 were not run: the TV went to standby twice during the hangs (see Odd things), and
LAPTOP paused to ask the user before waking it again. I'll finish them on request, or on the
next build.

Build **build-23** (59fbacb, IPA sha256 fc741cee…). Instances: app pid 1164 (prep, steps 0 and
1a) and pid 1182 (1b, 1c). TV time 04:49–05:25 CEST.

## Prep
- `laptop/refs/guest/*.sh` were copied to `$ROOTFS_DIR/opt/rl/refs/` (+x) and the manifest
  rebuilt: 70,388 entries (+12), `rltvos-assets` restarted.
- `install --tag build-23`: 14 s. `launch --fresh` woke the TV itself.
- `jit`: ready (4.38 s). `vfs mount`: ok.
- `fex init` → **`"allocator":"system"`** (fex-init.json).

## Results
| step | result | files |
|---|---|---|
| 0 VA budget | **PASS.** `none_256mb` 7.5 GB fresh → 7.25 after 4 guests → 7.25 after 6 (64 MB steps: 7.5 / 7.44 / 7.44). All six `/usr/bin/env` runs exit 0 with the 7 expected variables (106 syscalls, 78–94 ms each). phys_footprint 191 → 197 MB | va-0-fresh, va-1-after-4, va-2-after-6.json, step0-env1…6.* |
| 1a `sh -c 'echo one \| tr o 0'` | **HANG** (tv.py exec timed out after 120 s, no output). Analysis below | step1a.*, hang-log.txt, hang-ps.json, hang-jthreadsinfo-1164.jsonl, hang-pc9-code.jsonl, hang-fex-status.json, hang-killall.json, hang-ps-after-killall.json |
| 1b `sh -c 'x=$(echo hi); echo got:$x; (echo sub; exit 3); echo rc=$?'` | **PASS**: `got:hi` / `sub` / `rc=3`, exit 0, 66 syscalls, 92 ms (instance 2) | step1b.* |
| 1c `sh -c 'sleep 1 & echo bg=$!; wait; echo waited'` | **HANG**: 120 s timeout, no output. Its log and ps were lost because the TV went to standby before I could read them; almost certainly the same loop (a fork child before `execve`) | step1c.* |
| 2–5 | not run (TV paused, see Odd things) | |

## Step 1a hang: fault loop between `cow_fault` and the unaligned fix-up
**What the log shows** (hang-log.txt, lines 825–852):
```
825 822797.6 [7] pipe2(0x11a3dbb20, 0x0) = 0
826 822798.1 [7] rt_sigprocmask(0, 0x107dc86f8, 0x11a3db990, 8) = 0
827 822798.5 kernel: pid 7 fork -> child pid 8 (snapshot)
828 822798.5 kernel: pid 8 tid 8 start rip=0x107cf4f82 rsp=0x11a3db990
829 822798.6 [8] set_robust_list(0x1036cca20, 24) = 0
830 822798.8 [8] rt_sigprocmask(2, 0x11a3db990, 0x0, 8) = 0
831 822799.6 [8] close(3) = 0
832 822799.7 [8] dup2(4, 1) = 1
833 822799.7 [8] close(4) = 0
834 822804.2 [8] write(1, "one\n", 4) = 4
835 822804.8 [8] exit_group(0) = 0
836 822804.8 kernel: pid 8 tid 8 exited code=0 after 7 syscalls, 6.5 ms
837 822805.0 [7] clone(0x1200011, 0x0, 0x0, 0x1036cca10, 0x0) = 8
838 822805.0 [7] rt_sigprocmask(2, 0x11a3db990, 0x0, 8) = 0
839 822805.0 [7] --- signal 17 delivered: handler=0x1037efce0 frame=0x11a3db538 rip=0x107c58d77
840 822805.2 [7] rt_sigreturn() = 0
841 822805.5 [7] close(4) = 0
842–845 [7] newfstatat("/usr/local/sbin/tr" … "/usr/sbin/tr") = -1 ENOENT, newfstatat("/usr/bin/tr") = 0
846 822805.7 [7] rt_sigprocmask(0, 0x107dc86f8, 0x11a3db990, 8) = 0
847 822806.0 kernel: pid 7 fork -> child pid 9 (snapshot)
848 822806.0 kernel: pid 9 tid 9 start rip=0x107cf4f82 rsp=0x11a3db990
849 822806.0 [9] set_robust_list(0x1036cca20, 24) = 0
850 822806.0 [9] rt_sigprocmask(2, 0x11a3db990, 0x0, 8) = 0
851 822806.0 [9] dup2(3, 0) = 0
852 822806.0 [9] close(3) = 0
```
After line 852, pid 9 makes no more syscalls (its next one would be `execve("/usr/bin/tr")`).

**What `ps` shows** (hang-ps.json): pid 7 and pid 9 both `running`. pid 9 has 4 syscalls,
3 fds, and its exe is still `/usr/bin/sh`.

**What the debugger shows** (`rltvos-jit gdb`, `jThreadsInfo`; hang-jthreadsinfo-1164.jsonl):
- **`guest 7/7`** is blocked in `__psynch_cvwait` (x16 = 305), the vfork wait. Fine.
- **`guest 9/9`** is stopped with **EXC_BAD_ACCESS, code 0x101 = EXC_ARM_DA_ALIGN, address
  0x11c8006af**, at JIT pc 0x10a0e2204 (pool 0x109bd8000–0x111bd8000).
- The code at that pc (hang-pc9-code.jsonl):
  `…200: d503201f nop` / `…204: 59000167 stlurh w7, [x11]` with x11 = 0x11c8006af.
  That is a 16-bit store-release to an odd address that crosses a 16-byte boundary. It is still
  **unpatched**: the `nop` is not yet `dmb` and the `stlurh` is not yet `sturh`.
- 0x11c8006af is inside the shell's brk heap (`brk 0x11c800000-0x120800000`, step1a.json),
  which is covered by fork's snapshot.

**The fix-up counter doesn't move:** `unaligned_fixups` stayed at **133** across samples taken
5 s apart during the hang (hang-fex-status.json), so FEX's fix-up path is never reached.

**Why:**
- `GuardHandler` (core/fex/src/rlfex.cpp:76) first calls `g_FaultHook` for any SIGSEGV/SIGBUS.
  That is `Kernel::host_fault_hook`, which ignores `sig`/`code` and returns
  `mm->cow_fault(addr)`.
- `AddressSpace::cow_fault` (core/kernel/src/mm.cpp:442) returns **true for every fault on a
  tracked page** of the active snapshot. The first time it saves the page; after that it only
  "refreshes a stale protection" and still returns true.
- The guard therefore returns ("retry the access"). The store is still unaligned, faults with
  DA_ALIGN again, `cow_fault` says true again, and so on forever.
- `HandleUnalignedAccess` (which would rewrite to `dmb` + `sturh`, the same thing that works for
  `ldapur` in 003) is never called. Unaligned stores into CoW pages happen in forked children
  constantly (glibc string/stdio code), so this blocks every fork that writes before exec.

**`ps --killall`** returned `stopped: 2` and marked pids 7 and 9 `dead`, but both still showed
`live_threads: 1` (hang-ps-after-killall.json): the spinning thread never reaches a syscall
boundary. I relaunched the app to continue.

**LAPTOP's suggestion (REPO decides):** make the hook claim a fault only when it really was a
copy-on-write write fault:
- skip SIGBUS with `si_code == BUS_ADRALN` (or ESR DFSC 0x21, alignment);
- have `cow_fault` return false when the page was already saved and the protection was already
  right.

Then the guard falls through to `HandleUnalignedAccess`. For the first unaligned write to a
still-protected page, resolve the copy-on-write, then retry: the retry faults as alignment only
and gets fixed up.

## Odd things
- **The TV went to standby twice, both times during a hang.**
  - The first was 79 s into step 1a (`host: did enter background` at log 901968; later
    `httpd: listener lost (errno=9); re-created on port 7777`, so build-23's listener fix works).
  - The second was during step 1c, around 05:25.
  - Together with 003's unexplained trip to the home screen at 04:02, a person may be switching
    off the TV. LAPTOP stopped waking the TV and asked the user.
- `tv.py launch` (without `--fresh`) woke the TV and brought back the same instance (pid 1164)
  with its log intact, which is how hang-log.txt was fetched.

## Files
verdict.md, fex-init.json, va-0-fresh.json, va-1-after-4.json, va-2-after-6.json,
step0-env1…6.json/-stdout.txt, step1a/1b/1c.json/-stdout.txt, hang-log.txt (+ .reply.json),
hang-ps.json, hang-jthreadsinfo-1164.jsonl, hang-pc9-code.jsonl, hang-fex-status.json,
hang-killall.json, hang-ps-after-killall.json, launch.json, relaunch-1/2.json, jit.json,
jit-2.json, vfs-mount.json, vfs-mount-2.json.

## Addendum (05:50–05:56 CEST; the user said to continue)
- **Step 1c evidence**: `tv.py launch` (without `--fresh`) woke the TV and brought instance 1182
  back, still hung (hang1c-ps.json).
  - pid 4 (sh) and pid 5 (its fork child, still `/usr/bin/sh`, 6 syscalls, last line
    `openat("/dev/null") = 0`) were `running`.
  - `jThreadsInfo` (hang1c-jthreadsinfo-1182.jsonl): `guest 4/4` is in `__psynch_cvwait` (the
    vfork wait). **`guest 5/5` is stopped on EXC_BAD_ACCESS / EXC_ARM_DA_ALIGN, address
    0x116a77f5c** (the snapshotted guest stack), at JIT pc 0x1063b7788.
  - The code there (hang1c-pc5-code.jsonl) is `d940014a ldapur x10, [x10]`, still unpatched,
    with the `nop` slot after it.
  - This is the same `cow_fault` loop as 1a, for a load this time. hang1c-log.txt is the log.
- **Step 2 attempt on build-23**: `tv.py exec --wait 120 -- /opt/rl/refs/c3.sh` returned
  "app not reachable" after 20 s. The TV was on, the app was gone, and there was no crash report
  at the next launch. Not investigated: REPO had already fixed the loop in build-25 and replaced
  steps 2–5 with request 005.
