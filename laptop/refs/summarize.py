#!/usr/bin/env python3
"""summarize.py — digest an strace -f -tt trace (plain or .gz).

Prints: process count and execve list, per-syscall counts, per-(syscall,errno)
error counts, ENOSYS list, unfinished/resumed pairs, and the set of files
opened (top by count). Used both for the laptop reference traces and for the
TV's guest log (same strace-ish format), so the two can be diffed:

    summarize.py refs/c2.trace.gz
    summarize.py --json refs/c2.trace.gz          # machine-readable
    summarize.py --diff refs/c2.trace.gz tv-log.txt  # syscalls the TV never issued / errors that differ
"""
import collections
import gzip
import json
import re
import sys

LINE = re.compile(r"^(?:\[pid\s+)?(?P<pid>\d+)\]?\s+(?:(?P<time>\d\d:\d\d:\d\d(?:\.\d+)?)\s+)?(?P<rest>.*)$")
CALL = re.compile(r"^(?P<name>[a-z_0-9]+)\((?P<args>.*)\)\s*=\s*(?P<ret>-?\d+|0x[0-9a-fA-F]+|\?)(?:\s+(?P<err>E[A-Z0-9]+)\b.*)?$")
UNFINISHED = re.compile(r"^(?P<name>[a-z_0-9]+)\((?P<args>.*)\s+<unfinished \.\.\.>$")
RESUMED = re.compile(r"^<\.\.\. (?P<name>[a-z_0-9]+) resumed>\s*(?P<args>.*?)\)\s*=\s*(?P<ret>-?\d+|0x[0-9a-fA-F]+|\?)(?:\s+(?P<err>E[A-Z0-9]+)\b.*)?$")
SIGNAL = re.compile(r"^---\s+(?P<sig>SIG[A-Z0-9]+)")
EXIT = re.compile(r"^\+\+\+ (exited with (?P<code>\d+)|killed by (?P<sig>SIG[A-Z0-9]+)).*\+\+\+$")
PATH = re.compile(r'"((?:[^"\\]|\\.)*)"')


def open_any(p):
    if p == "-":
        return sys.stdin
    if p.endswith(".gz"):
        return gzip.open(p, "rt", errors="replace")
    return open(p, "rt", errors="replace")


def analyse(path):
    calls = collections.Counter()
    errors = collections.Counter()
    enosys = collections.Counter()
    signals = collections.Counter()
    exits = {}
    pids = set()
    execs = []
    opened = collections.Counter()
    pending = {}  # pid -> (name, args)
    n = 0
    first_t = last_t = None
    with open_any(path) as f:
        for raw in f:
            raw = raw.rstrip("\n")
            m = LINE.match(raw)
            if not m:
                # strace without -f has no pid column; try as rest only
                pid, t, rest = "0", None, raw
            else:
                pid, t, rest = m.group("pid"), m.group("time"), m.group("rest")
            if t:
                first_t = first_t or t
                last_t = t
            pids.add(pid)
            n += 1
            mu = UNFINISHED.match(rest)
            if mu:
                pending[pid] = (mu.group("name"), mu.group("args"))
                continue
            mr = RESUMED.match(rest)
            if mr:
                name = mr.group("name")
                args0 = pending.pop(pid, (name, ""))[1]
                args = args0 + mr.group("args")
                ret, err = mr.group("ret"), mr.group("err")
            else:
                mc = CALL.match(rest)
                if mc:
                    name, args, ret, err = mc.group("name"), mc.group("args"), mc.group("ret"), mc.group("err")
                else:
                    ms = SIGNAL.match(rest)
                    if ms:
                        signals[ms.group("sig")] += 1
                        continue
                    me = EXIT.match(rest)
                    if me:
                        exits[pid] = me.group("code") if me.group("code") is not None else me.group("sig")
                    continue
            calls[name] += 1
            if err:
                errors[(name, err)] += 1
                if err == "ENOSYS":
                    enosys[name] += 1
            if name == "execve":
                pm = PATH.search(args)
                if pm:
                    execs.append((pid, pm.group(1)))
            if name in ("open", "openat", "stat", "lstat", "newfstatat", "access", "readlink", "readlinkat", "statx", "faccessat", "faccessat2"):
                pm = PATH.search(args)
                if pm:
                    opened[(name, pm.group(1), err or "ok")] += 1
    return {
        "file": path, "lines": n, "first_time": first_t, "last_time": last_t,
        "processes": len(pids), "pids": sorted(pids, key=int)[:200],
        "execve": execs, "calls": dict(calls.most_common()),
        "errors": {f"{k[0]} {k[1]}": v for k, v in errors.most_common()},
        "enosys": dict(enosys), "signals": dict(signals), "exits": exits,
        "paths": [{"call": k[0], "path": k[1], "result": k[2], "n": v} for k, v in opened.most_common(400)],
    }


def render(r):
    out = []
    out.append(f"trace: {r['file']}  lines={r['lines']} processes={r['processes']} time={r['first_time']}..{r['last_time']}")
    out.append(f"unique syscalls: {len(r['calls'])}   ENOSYS: {', '.join(f'{k}x{v}' for k, v in r['enosys'].items()) or 'none'}")
    out.append("execve: " + "; ".join(f"{p}:{e}" for p, e in r["execve"][:40]))
    out.append("exits: " + ", ".join(f"{p}={c}" for p, c in list(r["exits"].items())[:40]))
    out.append("signals: " + ", ".join(f"{k}x{v}" for k, v in r["signals"].items()))
    out.append("")
    out.append("syscall counts:")
    for k, v in r["calls"].items():
        out.append(f"  {v:8d}  {k}")
    out.append("")
    out.append("errors (syscall errno count):")
    for k, v in r["errors"].items():
        out.append(f"  {v:8d}  {k}")
    out.append("")
    out.append("paths (top):")
    for e in r["paths"][:120]:
        out.append(f"  {e['n']:6d}  {e['call']:10s} {e['result']:8s} {e['path']}")
    return "\n".join(out)


def diff(ref, other):
    a, b = analyse(ref), analyse(other)
    out = [f"reference: {ref}", f"other:     {other}", ""]
    missing = [k for k in a["calls"] if k not in b["calls"]]
    extra = [k for k in b["calls"] if k not in a["calls"]]
    out.append("syscalls in reference but never in other: " + ", ".join(missing))
    out.append("syscalls in other but not in reference:   " + ", ".join(extra))
    out.append("")
    out.append("errors only in other (candidate bugs):")
    for k, v in b["errors"].items():
        if k not in a["errors"]:
            out.append(f"  {v:8d}  {k}")
    out.append("")
    out.append("errors only in reference (other returns success where Linux failed):")
    for k, v in a["errors"].items():
        if k not in b["errors"]:
            out.append(f"  {v:8d}  {k}")
    out.append("")
    ra = {(e["call"], e["path"]): e["result"] for e in a["paths"]}
    rb = {(e["call"], e["path"]): e["result"] for e in b["paths"]}
    out.append("path results that differ:")
    for k, v in ra.items():
        if k in rb and rb[k] != v:
            out.append(f"  {k[0]:10s} {k[1]}: ref={v} other={rb[k]}")
    return "\n".join(out)


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if argv[0] == "--json":
        print(json.dumps(analyse(argv[1]), indent=1))
        return 0
    if argv[0] == "--diff":
        print(diff(argv[1], argv[2]))
        return 0
    for p in argv:
        print(render(analyse(p)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
