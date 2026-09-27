#!/bin/sh
# 45-elf-audit.sh — list every x86-64 ELF *executable* in the rootfs that is
# not position independent (ET_EXEC) together with its lowest PT_LOAD
# address. Such binaries cannot run on the TV: guest address == host address
# and XNU keeps the low 4 GB of an arm64 process unmappable (hard page zero),
# so an image linked at 0x400000 has nowhere to go. Everything the guest
# executes must be PIE (ET_DYN); shared libraries always are.
#
# Usage: laptop/setup/45-elf-audit.sh [ROOTFS_DIR] > elf-audit.txt
# Output: one line per offender "ET_EXEC <lowest vaddr> <path>", then a summary.
# Read-only; needs only python3.
set -eu
. "$(dirname "$0")/lib.sh"
root=${1:-$ROOTFS_DIR}
[ -d "$root" ] || die "rootfs not found: $root"

python3 - "$root" <<'PY'
import os, struct, sys

root = sys.argv[1]
skip_dirs = {"proc", "sys", "dev", "run", "tmp", "var/cache", "usr/share/doc", "usr/share/man", "usr/share/locale"}
et_exec, et_dyn_exec, scanned = [], 0, 0

def lowest_load(f, e_phoff, e_phentsize, e_phnum):
    lo = None
    f.seek(e_phoff)
    for _ in range(e_phnum):
        ph = f.read(e_phentsize)
        if len(ph) < 56:
            break
        p_type, p_flags, p_offset, p_vaddr = struct.unpack_from("<IIQQ", ph, 0)
        if p_type == 1:  # PT_LOAD
            lo = p_vaddr if lo is None else min(lo, p_vaddr)
    return lo

for dirpath, dirnames, filenames in os.walk(root):
    rel = os.path.relpath(dirpath, root)
    if rel != "." and any(rel == d or rel.startswith(d + "/") for d in skip_dirs):
        dirnames[:] = []
        continue
    for name in filenames:
        path = os.path.join(dirpath, name)
        try:
            st = os.lstat(path)
        except OSError:
            continue
        if not os.path.isfile(path) or os.path.islink(path) or not (st.st_mode & 0o111):
            continue
        try:
            with open(path, "rb") as f:
                hdr = f.read(64)
                if len(hdr) < 64 or hdr[:4] != b"\x7fELF" or hdr[4] != 2:
                    continue
                e_type, e_machine = struct.unpack_from("<HH", hdr, 16)
                if e_machine != 62:  # EM_X86_64
                    continue
                scanned += 1
                e_phoff, = struct.unpack_from("<Q", hdr, 32)
                e_phentsize, e_phnum = struct.unpack_from("<HH", hdr, 54)
                if e_type == 2:  # ET_EXEC
                    lo = lowest_load(f, e_phoff, e_phentsize, e_phnum)
                    et_exec.append((lo or 0, "/" + os.path.relpath(path, root)))
                elif e_type == 3:
                    et_dyn_exec += 1
        except OSError:
            continue

for lo, p in sorted(et_exec, key=lambda t: t[1]):
    print("ET_EXEC 0x%x %s" % (lo, p))
below = sum(1 for lo, _ in et_exec if lo < 0x100000000)
print("summary: %d x86-64 ELF executables scanned, %d ET_DYN (ok), %d ET_EXEC (%d below 4 GB: cannot run on the TV)"
      % (scanned, et_dyn_exec, len(et_exec), below))
PY
