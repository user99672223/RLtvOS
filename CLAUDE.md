# RLtvOS — project brief (verbatim from the user; the contract for both sessions)

You are the REPO session of a two-session project. A second Claude Code
session ("LAPTOP") runs on the user's x86-64 Debian laptop on the same LAN
as the target device; it alone can touch the Apple TV, the game install,
atvloadly, JIT, the rootfs and strace. You do everything else: all code,
the tvOS app, the GitHub Actions build, and the scripts LAPTOP executes.
Write this whole message to CLAUDE.md as your first commit.

Project: run the PC build of Rocket League (Epic version, installed on the
laptop via Heroic/Legendary) on an Apple TV 4K 3rd gen (A15, 4 GB RAM).
Offline only, launched without Easy Anti-Cheat. Hobby research project:
done = an exhibition match vs bots running on the TV.

Architecture:
- One sandboxed tvOS app = one host process. Inside it, stock x86-64 Linux
  userspace binaries run unmodified: glibc, Xvfb, plain WineHQ wine64 (not
  Proton — its container layer needs kernel namespaces), DXVK (Gcenx's
  dxvk-macOS builds, which target MoltenVK), and RocketLeague.exe.
- CPU: FEXCore (vendored) with a new Darwin/tvOS platform layer. FEX has two
  host layers — Linux, and Windows (ntdll-only); model the Darwin one on the
  Windows one: thread creation, TLS, memory allocation, signal/exception
  delegator over sigaction+sigaltstack with Darwin's ucontext layout. JIT
  memory = RWX mmap, legal on this box because a debugger is attached
  (get-task-allow). Strong-memory-ordering mode on. Confirm the emitter
  never allocates x18 (reserved on Darwin).
- Kernel ("the fake kernel"): the Linux x86-64 syscall ABI implemented on
  XNU, plugged into FEXCore's SyscallHandler interface. Reuse FEX's ELF
  loader and syscall tables; replace every pass-through-to-host
  implementation. Guest address = host address. A guest process = a thread
  group inside our one process with its own reserved VA slice, fd table,
  pid, cwd, brk, signal state. fork = vfork semantics (parent suspended,
  child shares the parent's slice until execve gives it a fresh slice;
  nested vfork must work — Wine double-forks). wineserver is started by our
  init script with -f -p before wine64, so wine connects instead of forking
  it. Skip wine64-preloader. 4 KB guest pages on 16 KB host pages: shadow
  per-4K protection table, apply the union to the host page.
- VFS: read-only Debian amd64 rootfs + Wine prefix + game directory served
  from the laptop over HTTP range requests with a manifest, block-cached
  (1 MB blocks) in the app's Caches directory with read-ahead; writable
  overlay for prefix, saves, config. Synthetic /proc, /sys, /dev (null,
  zero, urandom, tty, shm, input/event*).
- Display: Xvfb in the guest (-extension MIT-SHM); stock winex11.drv;
  keyboard/mouse via XTEST (xdotool as a guest process). The X framebuffer
  is never shown.
- Graphics: FEX's Vulkan thunk — guest libvulkan.so.1 (x86-64, built on the
  laptop) → host thunk in the app → MoltenVK tvOS xcframework. Every X11
  surface request becomes VK_EXT_metal_surface on the app's CAMetalLayer.
- Controllers: guest SDL2 evdev backend (SDL_JOYSTICK_DISABLE_UDEV=1)
  reading synthetic /dev/input/eventN fed from GameController.framework;
  Wine's winebus picks them up.
- Audio: later. Wine runs with no audio driver.

Fixed facts: memory cap ~2085 MB phys_footprint, kill on exceed; probe
reservable VA on day one (mmap PROT_NONE in 1 GB steps) — it sets slice
sizes. No fork/exec, no Hypervisor.framework, no document picker, Caches
only (purgeable). Disable the tvOS idle timer while running. Free Apple ID;
never depend on paid entitlements; never update tvOS. EAC's files are
never loaded; don't touch them. Laptop-specific values (IPs, device id,
paths, JIT command, tvOS version) live in laptop/config.env, which LAPTOP
owns — read it, never edit it.

Handoff protocol:
- handoff/wait.sh (create it in your first commit):
    #!/bin/sh
    # usage: handoff/wait.sh requests|results  — blocks until the other
    # side pushes something under handoff/<arg>/, then prints the new files
    dir=handoff/$1; last=$(git rev-parse HEAD)
    while :; do
      git fetch -q origin main
      if [ -n "$(git diff --name-only "$last" origin/main -- "$dir")" ]; then
        git pull -q --rebase origin main
        git diff --name-only "$last" HEAD -- "$dir"; exit 0
      fi
      sleep 60
    done
  You wait with `handoff/wait.sh results` run as a background task and act
  when it returns; LAPTOP waits on `requests`. Also keep handoff/NEXT.md
  ("## repo" / "## laptop", each side edits only its own) current so a
  restarted session resumes; read it on start.
- handoff/requests/NNN-name.md — yours: build tag, exact tools/tv.py
  commands, what to capture, pass criteria. One open request at a time.
- handoff/results/NNN-name/ — LAPTOP's: verdict.md (PASS/FAIL + what was
  seen on the TV), screenshots, log.txt, mem.json, requested files.
- handoff/issues/NNN-name.md — either side, for blockers outside the
  current request. handoff/DECISIONS.md — one dated line per deviation.
- Never edit the other side's files. git pull --rebase before every push.
  Commit messages start with [repo].
- Builds: the Actions workflow builds on every push to main and attaches
  app.ipa to a release tagged build-<run_number>; wait for the release to
  exist before writing a request that names it.

Skeleton (fixed): one sandboxed tvOS app; FEXCore JIT with a Darwin layer;
fake Linux kernel on XNU with guest processes as thread groups; stock
Debian rootfs + stock Wine + DXVK + Xvfb + the game, served from the
laptop; Vulkan → MoltenVK thunk; offline, no EAC; this handoff. Everything
below the skeleton is an implementation choice and changes freely when the
box disagrees — futex primitive, page-protection strategy, vfork details,
rootfs distro, DXVK version, thunk mechanics, harness endpoints, checkpoint
order. Log each change in DECISIONS.md. If a skeleton item itself looks
impossible, do not work around it silently: write an issue with the
evidence and stop for the user.

Phase A — harness (first, small):
1. .github/workflows/build.yml: macOS runner, stable Xcode, tvOS build with
   CODE_SIGNING_ALLOWED=NO, zip Payload/*.app into app.ipa, attach to
   release build-<run_number>. Entitlements: get-task-allow=true.
2. tools/tv.py (reads laptop/config.env): build, install (atvloadly MCP or
   its HTTP endpoints), jit, launch/kill (atvremote), shot, log, mem, va,
   input, run, crash, cycle (install→jit→launch→shot→mem). Idempotent,
   JSON output.
3. In-app debug server (Swift, LAN only, port 7777): GET /status,
   /screenshot (PNG of the current Metal drawable), /log?since=N (host log
   + guest syscall log in strace format), /mem (phys_footprint via
   task_info(TASK_VM_INFO), os_proc_available_memory), /va, /crash
   (backtrace from an in-process signal handler); POST /input, POST /run
   {argv, env, cwd}, POST /kill.
4. Request 001: install, cycle, return screenshot showing memory numbers,
   VA number, "JIT ok" (executed code written into an RWX page).

Phase B — FEXCore builds for tvOS with the Darwin layer and executes a bare
x86-64 function with no kernel behind it; result on screen.

Phase C — fake kernel, one request per checkpoint, each ending in a TV
screenshot of the program's output:
C1 static x86-64 hello → write/exit_group.
C2 dynamic glibc hello → ld.so path: openat/read/mmap/mprotect/arch_prctl/
   brk/set_tid_address/rseq(ENOSYS)/clone3(ENOSYS→clone)/clock_gettime.
C3 busybox sh pipeline with a background job → vfork-fork/execve/pipe2/
   dup3/wait4/kill/rt_sig*/sigaltstack, async signals into a JIT'd thread
   via FEX's deferred-signal path.
C4 Xvfb :0 + xdpyinfo + xeyes → unix sockets, poll/select/epoll→kqueue,
   SO_PEERCRED, fcntl locks emulated per guest process.
C5 wine64 notepad under Xvfb → wineserver -f -p, the wineboot chain
   (services.exe, winedevice, plugplay, rpcss, explorer), /proc/self/{maps,
   exe,status,fd}, /proc/<pid>/mem (memcpy — same host task), futex on
   os_sync_wait_on_address (fallback __ulock_wait/__ulock_wake), eventfd,
   memfd_create, /dev/shm, uname/sysinfo/sched_getaffinity, tgkill →
   pthread_kill. Minimal ptrace only if the trace shows Wine needs it.
Phase D — D1 vkcube via the Vulkan thunk. D2 a DXVK d3d11 sample under
wine64. D3 a synthetic evdev device visible to evtest, driven from a paired
controller.
Phase E — E1 RocketLeague.exe (no EAC, 720p, textures low, effects off,
30 fps cap, -nomovie) reaches the main menu. E2 exhibition match vs bots.
Record fps and peak /mem for each in PROGRESS.md.
Memory work only when a step is killed by jetsam, in order: reserve slices
PROT_NONE and commit lazily; cap the JIT code cache and file-back it;
file-back large anonymous guest mappings so dirty pages are reclaimable;
then game settings. Measure every change with /mem; keep only what helps.

Order of work:
1. Commit CLAUDE.md, handoff/ (wait.sh, NEXT.md, DECISIONS.md, empty
   requests/ results/ issues/), PROGRESS.md.
2. Phase A. Push, wait for the release, write request 001, run
   `handoff/wait.sh results` in the background.
3. While waiting: write laptop/setup/*.sh — rootfs build, Wine prefix +
   DXVK + game, assets server (HTTP with Range support + manifest of path,
   size, sha256), and one strace-reference script per checkpoint C1–E2
   that runs it natively in the chroot under `strace -f -tt -o
   refs/<checkpoint>.trace.gz` (gzipped). LAPTOP runs them and reports in
   handoff/results/.
4. Phase B onward, one checkpoint per request. Before each change, read the
   result's guest log and the matching laptop trace; every ENOSYS or wrong
   return is the next task. Nothing is done without a PASS from the TV.
Ask the user only for what only they can supply; otherwise decide, log it
in DECISIONS.md, continue.

---

# Repo-session working notes (REPO owns this section)

## Shared branch

The repository started empty and this REPO session is restricted by its
harness to the branch `claude/rocket-league-apple-tv-zek0mi`. Until the
user creates `main` from it (or explicitly allows REPO to push `main`),
that branch is the shared integration branch:

- `handoff/wait.sh` follows `$HANDOFF_BRANCH`, else the checked-out
  branch's upstream, else `main` — so both sessions work on whichever
  branch they cloned.
- `.github/workflows/build.yml` builds on pushes to `main` and `claude/**`.
- LAPTOP: clone with
  `git clone -b claude/rocket-league-apple-tv-zek0mi <repo>` until `main`
  exists; commit messages start with `[laptop]`.

See handoff/DECISIONS.md for the dated entry.

## Layout

- `app/` — the tvOS app (XcodeGen `project.yml`, sources in `app/Sources`,
  C in `app/Sources/Native`). Built only by CI (no Xcode here).
- `tools/tv.py` — LAPTOP's driver; reads `laptop/config.env`
  (documented in `laptop/config.env.example`, which REPO owns).
- `laptop/setup/*.sh`, `laptop/refs/*.sh`, `laptop/assets_server.py` —
  scripts LAPTOP executes. `laptop/config.env` is LAPTOP's, never edited
  by REPO.
- `handoff/` — protocol files as described above.
- `PROGRESS.md` — checkpoint table with fps / peak `/mem`.

## Conventions

- One open request at a time; a request names a release tag that exists.
- Every deviation from the brief: one dated line in `handoff/DECISIONS.md`.
- Nothing is marked done in `PROGRESS.md` without a PASS in
  `handoff/results/`.
