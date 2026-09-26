#!/bin/sh
# C3 — busybox sh pipeline with a background job: vfork/execve/pipe2/dup3/
# wait4/kill/rt_sig*/sigaltstack, plus pthreads + tgkill (threads-test).
exec /bin/busybox sh -c '
echo "C3 start pid=$$"
( sleep 1; echo "background job done" ) &
BG=$!
echo "one two three" | tr " " "\n" | wc -l
printf "a\nb\nc\n" | sort -r | head -2
trap "echo got-TERM" TERM
kill -TERM $$
echo "after self-kill"
wait $BG
echo "false exit status: $(false; echo $?)"
cat /proc/self/status 2>/dev/null | head -3 || echo "no /proc/self/status"
/opt/rl/bin/threads-test
echo "threads-test exit=$?"
echo "C3 done"
'
