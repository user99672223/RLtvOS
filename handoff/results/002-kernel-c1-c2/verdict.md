# 002-kernel-c1-c2 — build-18 on the Apple TV

**Verdict: PARTIAL.** FEX self-test **7/7**, vaprobe2, VFS mount, all three dry runs and
**C1 PASS** (exact output, exit 0). **C2 is the first step to deviate:** `hello-dyn` dies inside
ld.so right after its first syscall (`brk`) with a host **SIGBUS**. The cause is an alignment
fault in FEX's TSO load `ldapur` on an unaligned 8-byte guest load (glibc `memcmp`). FEX's
Linux frontend fixes this up in its SIGBUS handler; rlfex's guard does not (details below).
The app stayed alive. The step 7 bonus was not run, because C2 failed. The step 8 two-context
diagnostic was not needed, because C1 passed after step 1.

Build: **build-18** (release body `FEX built: true, FEX linked: true`; IPA sha256 e763d4c1…).
App pid 1104. TV steps ran 03:36–03:43 CEST.

## Prep (laptop)
1. **Rootfs refresh**, done rootless by hand as the request allows. `apt-get download busybox`
   (trixie 1:1.37.0-6+b9) on the host, then inside the rootfs as fake root (lib.sh
   `rootfs_fakeroot_run`): `dpkg --purge busybox-static && dpkg -i busybox.deb`, then fresh
   `/opt/rl/src` + `sh /opt/rl/customize.sh`. I did not use the full `10-rootfs.sh` refresh: it
   re-runs apt for the whole package set, which could move wine/mesa away from the refs.
   - `file`: hello-static is `pie executable … static-pie linked`; busybox is `pie executable …
     dynamically linked`.
   - Run natively in the rootfs: hello-static, hello-dyn, `busybox echo busybox ok` and
     `sh -c 'echo shell ok; echo $0 $$'` all print the expected lines.
2. **elf-audit.txt**: 719 x86-64 executables, 625 PIE, **94 ET_EXEC**, all at 0x400000.
   - They are only compilers (gcc-14 `cc1`/`lto1`/`collect2`/…, the i686/x86_64 mingw-w64
     cross tools) and `/usr/bin/python3.13`.
   - **No blocker.** `wine`, `wineserver`, `Xvfb`, `xdotool`, `dash`, coreutils (`ls`),
     `xdpyinfo`, `xeyes`, `vkcube`, `evtest` and `strace` are all PIE. Wine 11 ships no
     `wine-preloader`.
3. Manifest rebuilt (`--manifest-only --rebuild`: 70,376 entries, 1.0 s via the hash cache),
   `rltvos-assets` restarted.
4. `tv.py install --tag build-18`: ok in **12 s** (the DDI was still mounted, so the 11-minute
   build-15 install is not simply caused by the DDI).
   - The first `tv.py launch --fresh` returned `"up": false`: the TV was in standby (see Odd
     things). It worked after `atvremote turn_on`.
   - `tv.py jit`: ok, `authorize_seconds 6.3`, pool 128 MB by debugger, `prepare_s 4.31`,
     attached/detached tests ok, stage `ready`.

## TV steps
| step | result | file |
|---|---|---|
| 1 `fex selftest` | **7/7 ok**: add 24, loop 5050, sse 42, call 42, mem 42, syscall 4242 (1 syscall), exit 7 (2 syscalls). Init 4.6 ms, FEX-59f85d6-rltvos, runs 0.05–2.0 ms | fex-selftest.json |
| 2 `va --probe2` | PROT_NONE 256 MB steps **6.25 GB**, 64 MB steps **6.5 GB**; RW, RW+NORESERVE and vm_allocate all 6.25 GB; stop errno 12 (vm_allocate kr 3). RW touched: stopped at 2 GB after 8 steps with errno 0 (presumably the probe's own cap). RLIMIT_AS/DATA unlimited. phys_footprint 168.4 MB | vaprobe2.json |
| 3 `vfs mount` | ok, 616 ms, 70,376 entries / 55,634 files / 48.19 GB | vfs-mount.json |
| 4 dry runs | all `ok:true`. hello-static: `dyn:true`, no interp, 6 VMAs (image ×4, `[stack]`, `[heap]`). hello-dyn and `/usr/bin/sh` (→ `/usr/bin/dash`): interp `/lib64/ld-linux-x86-64.so.2` mapped as `/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2`, 10 VMAs | dry-*.json |
| 5 **C1** | **PASS**: `hello from x86-64 static` / `333833500`, exit 0, pid 4, 3 syscalls, 2.8 ms. The screenshot's KERN line shows `last: pid 4 /opt/rl/bin/hello-static exited exit=0` | c1.json, c1-stdout.txt, c1-log.txt, c1-shot.png, c1-mem.json |
| 6 **C2** | **FAIL**: no output, **exit 138** (128 + Darwin SIGBUS 10), pid 5, 1 syscall, 34.5 ms. The app is alive. KERN line: `last: pid 5 /opt/rl/bin/hello-dyn exited exit=138` | c2.json, c2-log.txt, c2-shot.png, c2-mem.json |
| 7 bonus | not run (C2 failed) | |
| 8 `ps` | 5 processes, all `exited`: pids 1–3 are the dry runs (exit 0, 0 syscalls), 4 is C1 (0, 3 syscalls), 5 is C2 (138, 1 syscall); ppid 1 for 2–5 | ps.json |

Memory: phys_footprint 168.4 MB (before the guests) → 193.4 MB after C1 → 197.5 MB after C2;
peak 201.4 MB; available 1900 MB. `tv.py exec` prints the guest output before its JSON; I
split that output into `cN-stdout.txt` and `cN.json`.

## C2: the deviation
The log from the C2 exec on (c2-log.txt lines 75–83, verbatim; this was pid 5's only strace line):
```
75 61692.1 run: exec /opt/rl/bin/hello-dyn cwd=/
76 61692.8 kernel: pid 5 exec /opt/rl/bin/hello-dyn (run) {"entry":"0x10178d2c0",… "interp":{"base":"0x101770000","entry":"0x10178d2c0","range":"0x101770000-0x1017a8000"} …}
77 61693.0 kernel: pid 5 tid 5 start rip=0x10178d2c0 rsp=0x16880bd90
78 61693.0 host: guest exec /opt/rl/bin/hello-dyn → {"ok":true,"pid":5,…}
79 61698.3 [5] brk(0x0) = 0x170334000
80 61704.2 run: ps cwd=/
81 61726.7 kernel: pid 5 tid 5 FAULT signal=10 pc=0x108c2b1e0 addr=0x10179c39a (guest rip=0x101795630)
82 62218.8 run: ps cwd=/
83 62233.2 run: guest-out 5 cwd=/
```
Analysis, with ld.so loaded at 0x101770000:
- **addr** = ld.so+0x2c39a, in `.rodata`: byte 10 of `"x86-64-v4:x86-64-v3:x86-64-v2"` (the
  glibc-hwcaps subdirectory list, which starts at +0x2c390), i.e. the start of `x86-64-v3`. That
  16K host page holds only `r--` 4K pages, so a *read* there cannot be a protection fault.
- **Host code at pc**, read with the debugger after the fault (`rltvos-jit peek`, attach → `m` →
  detach; jit-peek-fault.json):
  ```
  0x108c2b1e0  d9400147  ldapur x7, [x10]        <- fault
  0x108c2b1e4  d503201f  nop                     (FEX's back-patch slot)
  0x108c2b1e8  d9400164  ldapur x4, [x11]
  0x108c2b1ec  d503201f  nop
  0x108c2b1f0  eb07009f  cmp x4, x7
  0x108c2b1f4  54000741  b.ne
  0x108c2b1f8  8b050154  add x20, x10, x5
  0x108c2b1fc  d95f8287  ldapur x7, [x20, #-8]
  ```
- With FEX's x64 static registers (rax=x4, rcx=x7, rdx=x5, rsi=x10, rdi=x11) this is
  **ld.so+0x23920**, glibc `memcmp`'s 8–15-byte path: `mov (%rsi),%rcx; mov (%rdi),%rax; cmp;
  jne; mov -0x8(%rsi,%rdx,1),%rcx`.
- rsi = 0x10179c39a, so the 8-byte load spans …39a–…3a1 and **crosses a 16-byte boundary**.
  In TSO mode FEX emits LDAPUR, which may be unaligned only within a 16-byte granule (LSE2).
  That gives an alignment fault, which Darwin delivers as SIGBUS.
- On Linux, FEX catches exactly this SIGBUS and calls
  `FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess()`
  (third_party/FEX/FEXCore/Source/Utils/ArchHelpers/Arm64.cpp:1933). That function back-patches
  the access (plain load + half-barrier in the `nop` slot) and resumes.
- In build-18, `GuardHandler` in core/fex/src/rlfex.cpp siglongjmps on *every* SIGBUS while a
  guest runs. Nothing in core/ or app/ calls `HandleUnalignedAccess`, so the fix-up never
  happens and the thread is reported as a guest FAULT.
- **The logged guest rip is stale.** 0x101795630 (ld.so+0x25630) is inside `strcmp`, because
  `CurrentFrame->State.rip` is only written at block exits. The faulting guest instruction is
  ld.so+0x23920.
- **LAPTOP's suggestion (REPO decides):** in the guard, before the longjmp, handle a SIGBUS as
  FEX's Linux signal delegator does:
  - when `CTX->IsAddressInCodeBuffer(Thread, pc)` holds, call
    `HandleUnalignedAccess(Thread, UnalignedHandlerType::HalfBarrier, pc, uc->uc_mcontext->__ss.__x)`;
  - if it returns a value, add it to the pc and return from the handler. For LDAPUR it returns
    0: the instruction was rewritten to LDUR, with `dmb ishld` in the nop slot, through
    `Allocator::GetWritableAddress(pc)` (the pool's RW alias), so it simply re-executes.
- glibc's string functions do unaligned 8-byte loads everywhere, so this blocks C2 and every
  later checkpoint.

## Odd things
- **TV standby:** `tv.py launch --fresh` (Companion `launch_app`) returns rc 0, but nothing
  starts while the TV sleeps (`power_state` Off); `tv.py jit` then says "app not reachable".
  LAPTOP now runs `atvremote turn_on` before launching (DECISIONS). REPO may want
  `tv.py launch` to do this itself.
- The build-18 install took 12 s with the DDI mounted; build-15 took 11 min. The slowness is
  intermittent.
- Each dry run takes a pid (1–3) and keeps ~72 MB of VA mapped (64 MB `[heap]` reservation +
  8 MB stack), since there is no reaping yet (known gap). C1 was pid 4 and C2 pid 5.
- The strace-format log prints raw newlines inside strings: `[4] write(1, "hello from x86-64
  static⏎", 25) = 25` spans two lines, whereas strace writes `\n`. This matters for line-based
  diffs against refs/.
- Log time stamps jump again (39578 → 61692 s within ~5 min of uptime).
- The console's CORE line says `exceptions=1` (unexplained; there was no app crash, and
  `crash_report=no`).

## Files
verdict.md, elf-audit.txt, fex-selftest.json, vaprobe2.json, vfs-mount.json, dry-hello-static.json,
dry-hello-dyn.json, dry-sh.json, c1.json, c1-stdout.txt, c1-log.txt (+ .reply.json), c1-shot.png,
c1-mem.json, c2.json, c2-log.txt (+ .reply.json), c2-shot.png, c2-mem.json, ps.json,
jit-peek-fault.json, launch.json, jit.json.
