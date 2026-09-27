# Issue 004 — feasibility proxies we should have measured on day one (LAPTOP, mostly laptop-only)

Filed by REPO 2026-09-27. Not a blocker for request 005; do it whenever the
laptop is free. Part A needs no TV time at all.

The two numbers that decide whether E1/E2 are reachable at all are the game's
memory footprint against the TV's ~2.1 GB kill limit and FEX's speed on the
A15. Both have cheap proxies available now; measuring them at D2/E1 would have
been weeks late.

## A. Game footprint, native, on the laptop (no TV)

The laptop's Intel GPU shares system RAM like the TV does, so the system-wide
`MemAvailable` delta includes what DXVK/Mesa allocate for textures and
buffers — the closest thing to the TV's unified-memory footprint we can get
before the game runs there. Use the S7 known-good setup at the target
settings (720p, textures low, effects off, 30 fps cap, `-nomovie`, offline):

1. `free -m` (idle laptop, Xvfb not running) → `mem-idle.txt`.
2. `OUT=... results/000-setup/s7-game.sh start` with the low settings applied
   (s7-lowsettings.py) and `DXVK_HUD=fps,memory` (the HUD prints DXVK's device
   memory use on screen).
3. At the **main menu** (wait ~30 s after it appears), and again **during an
   exhibition match vs bots** (~60 s in), in another shell:
   ```sh
   free -m
   for p in $(pgrep -f 'RocketLeague.exe|wineserver|winedevice|services.exe|explorer.exe|plugplay|rpcss|Xvfb'); do
     printf '%s %s ' "$p" "$(tr '\0' ' ' < /proc/$p/cmdline | cut -c1-40)"; grep -E '^(Rss|Pss|Swap):' /proc/$p/smaps_rollup | tr '\n' ' '; echo
   done
   ```
   plus `s7-game.sh shot menu` / `shot match` so the DXVK HUD memory line is
   captured. Save as `mem-menu.txt`, `procs-menu.txt`, `mem-match.txt`,
   `procs-match.txt`, `menu.png`, `match.png`.
4. Report, per state: **sum of Pss** over the wine processes, **MemAvailable
   delta** versus idle, DXVK device memory from the HUD, and fps.

Interpretation (mine, for the record): the TV budget is ~2085 MB phys_footprint
with the 128 MB JIT pool always resident and FEX's code caches on top (64–256
MB). If the native sum of Pss + GPU allocations at the main menu is already
above ~1.6 GB, E1 is at risk and we should say so before building D1–D3; below
~1.2 GB it is plausible; in between it depends on the memory work listed in
the brief.

## B. FEX speed proxy (needs the TV, next time it is on — request 006 will include it)

`laptop/refs/guest/src/cpubench.c` (built by `rootfs-customize.sh` into
`/opt/rl/bin/cpubench`; re-run the customize step and rebuild the manifest) runs
seven fixed-work tests — integer ALU, scalar FP, SSE loops, memcpy, random
memory reads, branchy code, calls — for about 7 s natively and prints ns per
operation plus a JSON line.

1. Native baseline in the rootfs: `laptop/rootfs_exec.sh -- /opt/rl/bin/cpubench`
   → `cpubench-laptop.txt` (note the laptop CPU model).
2. On the TV: `tv.py exec --wait 300 -- /opt/rl/bin/cpubench` → `cpubench-tv.txt`
   (the first run includes translation; run it twice and keep both).
3. The per-test ratio TV/laptop is the slowdown per kind of code. Rocket
   League's frame is mostly scalar/SSE FP and branchy game logic; a ratio
   above ~6 on those (versus a laptop of RL's minimum spec) makes 30 fps
   unlikely before any graphics cost is counted.

## Files
`handoff/results/issue-004/`: `mem-idle.txt`, `mem-menu.txt`, `procs-menu.txt`,
`mem-match.txt`, `procs-match.txt`, `menu.png`, `match.png`,
`cpubench-laptop.txt`, `cpubench-tv.txt` (when available), `notes.md` with the
four numbers per state and the laptop's CPU/GPU model.
