# Issue 004 — feasibility proxies (LAPTOP, 2026-09-27 06:06–06:13 CEST)

Laptop: **Intel Core i5-1135G7** (Tiger Lake, 4C/8T, 2.4 GHz base / 4.2 GHz turbo, intel_pstate
`powersave`), **Intel Iris Xe (TGL GT2)**, which shares system RAM like the TV. 15.4 GiB RAM,
Debian 13, kernel 6.12.

## A. Native game footprint (S7 setup: offline, `-nomovie -noeac`, 1280x720, low preset, 30 fps cap)
Setup:
- `NONET=1 DXVK_HUD=fps,memory results/000-setup/s7-game.sh start`, with `s7-lowsettings.py`
  run first (it had to reset 4 settings the game had changed).
- Textures are at low: every `TEXTUREGROUP_*` has `MaxLODSize=256` except UI/Cinematic.
- Menu measured ~30 s after the main menu appeared. Match: exhibition 1v1 Soccar vs a Rookie bot,
  measured ~60 s after kickoff.

| state | Pss sum, Wine processes | of which RocketLeague.exe | MemAvailable Δ vs idle | shmem Δ (≈ GPU buffers) | DXVK HUD `Vidmem heap 0` | fps |
|---|---|---|---|---|---|---|
| idle | — | — | (10,001 MB available) | (1,705 MB) | — | — |
| **main menu** | **4,218 MB** | 4,109 MB (Anonymous 4,041 MB, file 66 MB) | **−4,909 MB** | +1,085 MB | 1,136 MB allocated (14 %), **560 MB used** | **29.9** |
| **match, ~60 s** | **4,385 MB** | 4,276 MB (Anonymous 4,204 MB, file 71 MB) | **−5,085 MB** | +1,098 MB | 1,136 MB allocated, **535 MB used** | **29.9** (30.5 at kickoff) |

- The other ~110 MB of Pss: Xvfb 37 MB and explorer.exe 34 MB (a Wine virtual desktop; see
  "Caveat"), plus wineserver, services, winedevice ×2, plugplay, svchost and rpcss.
- **RocketLeague.exe is ~97 % private, dirty, anonymous memory** (rollup-*.txt, top-maps-menu.txt):
  - at the menu: `Private_Dirty 4,093,436 kB`, `Pss_Anon 4,138,416 kB`, `Pss_File 68,052 kB`,
    `Pss_Shmem 938 kB`, `AnonHugePages 299,008 kB`;
  - by mapping: 4.06 GB anonymous and 62 MB `[heap]`; the next largest is `libLLVM.so.19` at
    57 MB Rss (Mesa's lavapipe ICD also loads).
  - It is not texture memory: the GPU side is the shmem delta and DXVK's heap, about 1.1 GB.
- Files: mem-idle.txt, mem-menu.txt, procs-menu.txt, rollup-menu.txt, top-maps-menu.txt,
  menu.png, mem-match.txt, procs-match.txt, rollup-match.txt, match.png. The DXVK HUD
  (fps + `Vidmem heap 0`) is in the top-left of both images.

**Against REPO's own thresholds:**
- The native main-menu footprint is **~4.2 GB Pss + ~1.1 GB GPU buffers**, versus "above
  ~1.6 GB → E1 at risk" and the TV's ~2.1 GB kill limit. That is before the 128 MB JIT pool and
  FEX's code caches.
- The game's own anonymous heap is ~2× the TV's whole budget, and textures are already capped at
  256 px, so settings alone can't close the gap.
- It is dirty anonymous memory, so file-backing clean pages won't reclaim it either. On the TV it
  could only go to the memory compressor, and phys_footprint counts compressed pages too.
- LAPTOP reads this as E1/E2 being at high risk as specified. REPO decides what to tell the user.

**Caveat:** Wine may inflate some of this versus Windows, and UE3 may size caches from the 15 GB
of RAM it sees. A cheap follow-up: rerun with the sandbox capped (cgroup `MemoryMax=2.5G` with
swap disabled) and see whether the game adapts or dies.

**Virtual desktop:** this run, and all earlier S7 runs, used a Wine virtual desktop
(`explorer /desktop=RL,--`). The cause was a bug in `laptop/rootfs_exec.sh`: an empty
`--env VDESK=` made bwrap take the next word, `--`, as the value. It is fixed now. The cost is
~34 MB (explorer.exe); the game's own numbers are unaffected. The E1/E2 strace refs used a
different launcher and were not affected.

## B. FEX speed proxy: native baseline
`laptop/rootfs_exec.sh -- /opt/rl/bin/cpubench`, rebuilt into the rootfs by `customize.sh`, with
the manifest rebuilt (70,394 entries) so the TV can run it. Two runs (cpubench-laptop.txt):

| test | run 1 | run 2 |
|---|---|---|
| int_alu | 2.57 ns/op | 2.55 |
| fp_scalar | 46.1 ns/step | 46.6 |
| simd | 0.127 ns/elem | 0.127 |
| memcpy | 16.3 GB/s | 16.9 |
| mem_random | 87.5 ns/read | 87.1 |
| branchy | 0.27 ns/step | 0.27 |
| calls | 0.37 ns/call | 0.38 |
| total | 2.44 s | 2.43 s |

`cpubench-tv.txt` will come with request 006: two runs on the TV, the first including
translation.
