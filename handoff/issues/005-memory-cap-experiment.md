# Issue 005 — E1/E2 feasibility: the game under a 1.5–3 GB memory cap with swap (laptop-only, decisive)

Filed by REPO, 2026-09-27, after `handoff/results/issue-004/notes.md`.

## Evidence (issue 004, native, laptop, 720p low, 30 fps cap)

| | Pss (Wine processes) | RocketLeague.exe private dirty anonymous | GPU buffers (shmem Δ) | DXVK vidmem used |
|---|---|---|---|---|
| main menu | 4,218 MB | 4,093 MB | +1,085 MB | 560 MB |
| match, 60 s | 4,385 MB | 4,260 MB | +1,098 MB | 535 MB |

The TV kills the app at ~2,085 MB `phys_footprint`. That figure counts anonymous
memory, compressed memory (so the compressor does not help), IOKit/Metal
allocations (so the GPU buffers count too) and page tables. The game's own
private heap is therefore **~2× the whole device budget before FEX, Wine, Xvfb,
the JIT code cache and the VFS cache**, and textures are already capped at 256
px, so settings cannot close the gap. As specified, E1/E2 look infeasible.

## The one mechanism that could still make it fit

`phys_footprint` does **not** count file-backed pages: a `MAP_SHARED` mapping of a
file in the app's Caches directory lives in the vnode's VM object (external
memory), dirty or clean, and XNU pages it in and out through the unified buffer
cache like any file. The brief already lists "file-back large anonymous guest
mappings" as the last memory step; it would have to become the *first* design
item instead: every large guest anonymous mapping (the game's heap, Wine's
views) backed by a sparse file in Caches, so the game's 4.2 GB lives outside the
footprint and the OS keeps as much of it resident as free RAM allows. The Apple
TV has 4 GB of RAM; after the system (~1 GB) and our own internal memory
(app + FEX + JIT cache + GPU buffers, ~1.2–1.5 GB), roughly **1.5–2 GB of page
cache** would hold the game's working set, the rest paging from NAND at 16 KB
granularity.

Whether the game tolerates that is measurable on the laptop today: a cgroup
memory cap with swap enabled is the same situation (anonymous pages beyond the
cap go to the swap device instead of a Caches file; NVMe is somewhat faster than
the TV's NAND, so a pass here is necessary, not sufficient).

## What to run (no TV needed; ~45 min)

Same S7 setup as issue 004 (offline, `-nomovie -noeac`, 1280x720, low, 30 fps
cap, `DXVK_HUD=fps,memory`), virtual desktop now off. Run as before but inside
a memory-limited cgroup. Record everything under
`handoff/results/issue-005/`.

### Prep
1. `swapon --show; cat /proc/swaps; cat /sys/module/zswap/parameters/enabled;
   lsblk -d -o NAME,MODEL,ROTA` → `swap.txt`. The swap must be a **disk** device
   (NVMe/SSD partition or file), not zram; if zswap is enabled, disable it for
   the runs (`echo 0 | sudo tee /sys/module/zswap/parameters/enabled`) so pages
   really go to disk. If the only swap is zram, add a 6 GB swap file on the SSD
   for the experiment (`fallocate -l 6G /swapfile.rl; chmod 600; mkswap; swapon`)
   and remove it afterwards.
2. Disable transparent huge pages for page-granular numbers:
   `echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled` (restore
   `madvise`/`always` afterwards; note the original value).
3. Create the cgroup (cgroup v2, Debian 13):
   ```sh
   sudo mkdir -p /sys/fs/cgroup/rl
   echo max | sudo tee /sys/fs/cgroup/rl/memory.swap.max
   ```
   The launching shell joins it, so every child (bwrap, Xvfb, wineserver, all
   Wine processes) inherits it: `echo $$ | sudo tee /sys/fs/cgroup/rl/cgroup.procs`.
   Verify after launch: `cat /proc/$(pgrep -f RocketLeague.exe)/cgroup` shows
   `/rl`. (`systemd-run --user --scope -p MemoryMax=… -p MemorySwapMax=infinity`
   is an alternative if the memory controller is delegated to your user slice;
   whichever one you use, the check is the same.)
4. In a second terminal during every run: `vmstat 5 > vmstat-<cap>.txt` and
   `while :; do date +%T; cat /sys/fs/cgroup/rl/memory.current /sys/fs/cgroup/rl/memory.swap.current; grep -E 'pgmajfault|workingset_refault_anon|pgscan|pgsteal' /sys/fs/cgroup/rl/memory.stat; sleep 5; done > cg-<cap>.txt`.

### A. Working set without a cap (how much of the 4.2 GB is hot)
With `memory.max = max`, at the main menu and again in a match (1v1 vs a
Rookie bot, after kickoff), for windows of 2 s, 10 s and 60 s:
```sh
pid=$(pgrep -f RocketLeague.exe)
echo 1 | sudo tee /proc/$pid/clear_refs; sleep <N>; grep -E '^(Rss|Referenced|Anonymous):' /proc/$pid/smaps_rollup
```
→ `ws-menu.txt`, `ws-match.txt` (the `Referenced` line after each window is
the memory touched in that window). Also once: `grep -E 'Referenced' /proc/$pid/smaps | awk '{s+=$2} END {print s}'` cross-check.

### B. Capped runs (the actual test)
For each cap in **3072M, 2048M, 1536M, 1024M** (stop descending once the game
dies or the match is below 10 fps):
1. `echo <cap> | sudo tee /sys/fs/cgroup/rl/memory.max`, launch the game from a
   shell inside the cgroup, wait for the main menu.
2. Note: time to main menu, fps at the menu after 30 s, `memory.current`,
   `memory.swap.current`.
3. Start the exhibition match (1v1, Rookie), play/idle ~90 s. Note: fps at
   kickoff, fps after 60 s (min/typical from the HUD; a screenshot of the HUD at
   60 s → `match-<cap>.png`), `memory.current`, `memory.swap.current`,
   `pgmajfault` delta over the 60 s, `vmstat` `si`/`so` (KB/s of swap-in/out)
   averaged over the match minute, and `memory.events` (`oom_kill` count).
4. Quit cleanly (or `s7-game.sh stop`) before the next cap; `swapoff -a && swapon -a`
   between runs to start from empty swap.

Summarise in `notes.md` as one table: cap | menu fps | menu resident | match
fps at 60 s | match resident | swap used | swap-in MB/s | majfaults/s | died?

### C. One extra data point if time allows
The same at 1536M with the game's audio disabled if the config allows it
(`TAGame/Config/TASystemSettings.ini` or the audio device set to none) — Wwise
soundbanks are a candidate for a large part of the heap, and offline play with
no audio is acceptable for E1/E2.

## Reading the result (REPO's thresholds)

- **1536M cap: match ≥ 25 fps, swap-in below ~50 MB/s sustained, no OOM** →
  file-backed guest memory is a plausible design; REPO moves it to the front
  of the plan (before D1) and E1/E2 stay the target.
- **2048M–3072M needed for ≥ 25 fps** → marginal: on the TV that is the whole
  page cache; E2 possible only with further cuts (audio, `dxvk.maxChunkSize`,
  JIT cache cap); the user decides.
- **≤ 15 fps or OOM at 2048M** → the game does not fit this device as
  specified. REPO reports it to the user with these numbers; D/E work stops
  unless the user picks a different target.

Until this result is in, D1–E2 are on hold; C3–C5 verification of already
built code continues (request 006), since it is independent of the outcome.
