#!/bin/sh
# C2 — dynamic glibc hello: ld.so path (openat/read/mmap/mprotect/arch_prctl/
# brk/set_tid_address/rseq/clone3-or-clone/clock_gettime).
/opt/rl/bin/hello-dyn
rc=$?
echo "hello-dyn exit=$rc"
# A second dynamic program with more libc surface (coreutils):
/bin/ls -la /opt/rl/bin
/usr/bin/env | sort | head -5
exit $rc
