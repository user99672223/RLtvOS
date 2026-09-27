#!/bin/sh
# D1 — vkcube through the Vulkan path (on the TV: guest libvulkan.so.1 →
# thunk → MoltenVK; on the laptop: whatever ICD works under Xvfb).
. "$(dirname "$0")/_lib.sh"
start_xvfb :0 || exit 1
vulkaninfo --summary 2>&1 | head -40 || echo "vulkaninfo failed"
timeout 40 vkcube --c "${RL_FRAMES:-300}" --width 800 --height 600 >"$OUT/vkcube.log" 2>&1 &
VC=$!
sleep 5
snap d1
wait $VC
echo "vkcube exit=$?"
tail -5 "$OUT/vkcube.log"
stop_xvfb
echo "D1 done"
