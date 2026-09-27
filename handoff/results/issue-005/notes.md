# Issue 005 — the game under a memory cap with swap (LAPTOP, 2026-09-27 06:43–07:06 CEST)

**Result: at a 1536 MB cap the exhibition match runs at 29.9–30.5 fps, swap-in averages
1.15 MB/s over the match minute (peak 6.6 MB/s), and there is no OOM.** That meets REPO's first
case ("file-backed guest memory is a plausible design; E1/E2 stay the target").
- 2048M and 3072M are just as smooth.
- **1024M is OOM-killed during start-up.** The floor is roughly 1–1.5 GB, and a large part of it
  is GPU buffers (shmem) that can't be swapped while in use.

## Setup (and how it differs from the issue's recipe)
- **Game:** same S7 setup as issue 004: offline (`NONET=1`), `-nomovie -noeac`, 1280x720, low
  preset, 30 fps cap, `DXVK_HUD=fps,memory`. The **virtual desktop is off** now (rootfs_exec.sh
  fix).
- **Match:** exhibition 1v1 Soccar vs a Rookie bot, random arena.
- **No sudo** (the user can't type into a terminal), so these are rootless equivalents:
  - **cgroup:** `systemd-run --user --scope --unit=rl-cap-<cap> -p MemoryMax=<cap>
    -p MemorySwapMax=infinity`. systemd 257 delegates cpu, memory and pids to
    `user@1000.service`. The game process was checked to be in
    `…/app.slice/rl-cap-<cap>.scope`.
  - **THP off for the game's process tree only:** `prctl(PR_SET_THP_DISABLE)` in a wrapper
    (`THP_enabled: 0` in the game's /proc status). The system setting stays `[always]`.
  - **swap.txt:** swap is a **16 GB NVMe partition** (`/dev/nvme0n1p3`, INTEL SSDPEKNW512G8H),
    not zram; **zswap = N**; swappiness 60.
  - **No `swapoff/swapon` between runs.** Each run is a new scope whose `memory.swap.current`
    starts at 0. About 1.5 GB of other processes' swap stays on the device.
- **Monitoring:** every 5 s, `memory.current`, `memory.swap.current`, `memory.stat` (pgmajfault,
  workingset_refault_anon, pgscan, pgsteal, anon/file/shmem) and `memory.events` oom_kill
  (cg-<cap>.txt), plus `vmstat 5` (vmstat-<cap>.txt; `-t` timestamps from 2048M on).
- **fps:** read from the DXVK HUD in screenshots at kickoff, +30 s and +60 s (match-<cap>.png is
  the +60 s one; menu-<cap>.png is the menu after 30 s).

## A. Working set, no cap (ws-menu.txt, ws-match.txt)
`clear_refs` = 1, wait, then `Referenced` of RocketLeague.exe (Rss 4.25 GB at the menu, 4.46 GB
in the match):

| window | main menu | match (from kickoff) |
|---|---|---|
| 2 s | 34 MB | 53 MB |
| 10 s | 43 MB | 74 MB |
| 60 s | 53 MB | 188 MB (includes goal replays) |

The smaps cross-check agrees (53,300 kB / 188,180 kB). The CPU touches only 1–4 % of the resident
heap per minute. Uncapped, `memory.current` was 5.66 GB at the menu and 5.92 GB in the match
(heap + page cache + GPU shmem).

## B. Capped runs
| cap | launch → title / menu | menu fps (+30 s) | menu resident / swap | match fps: kickoff / +30 s / +60 s | match resident | swap used | swap-in over the match minute (avg / peak) | major faults in the match minute | died? |
|---|---|---|---|---|---|---|---|---|---|
| none | ≤30 s / 65 s | 30.0 | 5,657 MB (all) / 0 | 29.9 at kickoff | 5,922 MB | 0 | 0 | 0 | no |
| **3072M** | ≤45 s / 74 s | **29.9** | 3,009 / 2,532 MB | **29.9 / 29.9 / 29.9** | 3,068–3,072 MB | 2.81 GB | 0.04 / 0.14 MB/s* | 100 (1.8/s) | no |
| **2048M** | ≤51 s / 78 s | **30.0** | 1,996 / 3,430 MB | **29.9 / 29.9 / 30.0** | 2,047–2,048 MB | 3.72 GB | 0.22 / 0.69 MB/s | 1,062 (18.6/s) | no |
| **1536M** | ≤63 s / 89 s | **30.0** | 1,485 / 3,898 MB | **29.9 / 30.0 / 30.5** | 1,535 MB | 4.31 GB | **1.15 / 6.6 MB/s** | 7,679 (135/s; 210/s in the first 30 s, 45/s after) | **no** (oom_kill 0; memory.events `max 31131`) |
| 1024M | — | — | — | — | — | peak 5.5 GB | — | — | **OOM-killed** 40 s after launch, while the title screen first rendered (oom-1024M.txt) |

\* The 3072M run's vmstat had no timestamps; its window is placed from the monitor start
(±10 s). The value is tiny either way.

Notes:
- **Launch → menu times** include ~25 s of scripted menu clicks. "Title" is an upper bound (the
  first screenshot). The slowdown from the caps is at load time (1536M: title by 63 s instead of
  ≤30 s; ~190k major faults during start-up). Once loaded, it plays smoothly.
- **1536M:** vmstat `si` over the match minute (KB/s, 5 s samples):
  290, 1788, 6782, 322, 1489, 74, 320, 1107, 136, 805, 34, 79. So the spike is loading the arena
  right after kickoff, then ≤1.5 MB/s.
- **1024M:** the last sample before the kill showed anon 426 MB + **shmem 609 MB** (DXVK/Mesa GEM
  buffers, which count against the cgroup and can't be swapped while the GPU uses them) + 5.5 GB
  swapped.
- **What the DXVK HUD shows:** `Vidmem heap 0` 1,136–1,184 MB allocated / 559–592 MB used,
  depending on the arena.

## Reading (LAPTOP's summary, REPO decides)
- REPO's criterion for 1536M (match ≥ 25 fps, swap-in < ~50 MB/s sustained, no OOM) is met with
  a large margin: ~30 fps, ~1 MB/s.
- The CPU working set is small (≤ 190 MB per minute), so a file-backed guest heap with 1.5–2 GB of
  page cache on the TV should behave like the 1536M run. The TV's NAND is slower than this NVMe,
  but the rates needed are tiny, except at load time (arena load at kickoff: a few MB/s for
  ~10 s).
- The floor is GPU memory. Below ~1.5 GB, the unswappable GPU buffers (~0.6 GB used / 1.1 GB
  allocated by DXVK here) plus the hot heap don't fit. On the TV, Metal allocations count toward
  phys_footprint too, so shrinking DXVK's allocations (chunk size, allocated vs used) is what
  still needs attention.
- **Not done:** part C (1536M with audio disabled), which isn't needed for the decision.
