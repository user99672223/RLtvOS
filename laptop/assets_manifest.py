#!/usr/bin/env python3
"""assets_manifest.py — LAPTOP: build the manifests the assets server publishes.

For each served root (rootfs, prefix, home, game) writes
  $ASSETS_DIR/manifests/<root>.json  and  <root>.json.gz
  $ASSETS_DIR/manifests/index.json   (roots, counts, bytes, URLs)

Entry format (superset of the brief's path/size/sha256):
  {"path": "usr/bin/ls", "type": "file", "mode": 493, "size": 151344, "sha256": "…", "mtime": 1700000000}
  {"path": "bin", "type": "symlink", "target": "usr/bin"}
  {"path": "usr", "type": "dir", "mode": 493}
  {"path": "…", "type": "other", "mode": …}          (fifos/sockets/devices: never served)
Paths are relative to the root, "/"-separated, sorted. File bytes are served at
  http://LAPTOP_IP:ASSETS_PORT/<root>/<path>  (HTTP Range supported).
The server follows symlinks on disk, so clients must resolve symlinks from the
manifest and never request a symlink path. sha256 values are cached by
(size, mtime_ns, inode) in manifests/.cache-<root>.json, so re-runs are cheap.

    assets_manifest.py [--roots rootfs,prefix,home,game] [--no-hash]
"""
import argparse
import datetime as dt
import gzip
import hashlib
import json
import os
import pathlib
import stat
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent


def load_env(path):
    env = {}
    if path.exists():
        for raw in path.read_text().splitlines():
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            v = v.strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                v = v[1:-1]
            else:
                v = v.split(" #", 1)[0].strip()
            env[k.strip()] = os.path.expanduser(os.path.expandvars(v))
    return env


CFG = load_env(ROOT / "laptop" / "config.env")
HOME = pathlib.Path.home()
ASSETS = pathlib.Path(CFG.get("ASSETS_DIR", HOME / "rltvos" / "assets"))
ROOTS = {
    "rootfs": pathlib.Path(CFG.get("ROOTFS_DIR", ASSETS / "rootfs")),
    "prefix": pathlib.Path(CFG.get("WINEPREFIX_DIR", ASSETS / "prefix")),
    "home": pathlib.Path(CFG.get("HOME_DIR", ASSETS / "home")),
    "game": pathlib.Path(CFG.get("GAME_DIR", "")),
}


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb", buffering=0) as f:
        while True:
            b = f.read(4 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def scan(name, base, do_hash):
    mdir = ASSETS / "manifests"
    mdir.mkdir(parents=True, exist_ok=True)
    cache_p = mdir / f".cache-{name}.json"
    try:
        cache = json.loads(cache_p.read_text())
    except Exception:
        cache = {}
    new_cache = {}
    entries = []
    nbytes = nfiles = hashed = unreadable = 0
    t0 = time.time()
    for dirpath, dirnames, filenames in os.walk(base, followlinks=False):
        dirnames.sort()
        rel_dir = os.path.relpath(dirpath, base)
        for nm in sorted(dirnames + filenames):
            full = os.path.join(dirpath, nm)
            rel = nm if rel_dir == "." else f"{rel_dir}/{nm}"
            try:
                st = os.lstat(full)
            except OSError:
                continue
            m = st.st_mode
            if stat.S_ISLNK(m):
                entries.append({"path": rel, "type": "symlink", "target": os.readlink(full)})
            elif stat.S_ISDIR(m):
                entries.append({"path": rel, "type": "dir", "mode": stat.S_IMODE(m)})
            elif stat.S_ISREG(m):
                e = {"path": rel, "type": "file", "mode": stat.S_IMODE(m), "size": st.st_size,
                     "mtime": int(st.st_mtime)}
                if do_hash:
                    key = f"{st.st_size}:{st.st_mtime_ns}:{st.st_ino}"
                    c = cache.get(rel)
                    if c and c[0] == key:
                        e["sha256"] = c[1]
                    else:
                        try:
                            e["sha256"] = sha256_file(full)
                            hashed += 1
                        except OSError:
                            unreadable += 1
                            e["unreadable"] = True
                    if "sha256" in e:
                        new_cache[rel] = [key, e["sha256"]]
                entries.append(e)
                nbytes += st.st_size
                nfiles += 1
            else:
                entries.append({"path": rel, "type": "other", "mode": stat.S_IMODE(m)})
    doc = {"root": name, "generated": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
           "url_prefix": f"/{name}/", "files": nfiles, "bytes": nbytes, "entries": entries}
    data = json.dumps(doc, separators=(",", ":")).encode()
    (mdir / f"{name}.json").write_bytes(data)
    (mdir / f"{name}.json.gz").write_bytes(gzip.compress(data, 6))
    cache_p.write_text(json.dumps(new_cache, separators=(",", ":")))
    return {"root": name, "dir": str(base), "entries": len(entries), "files": nfiles, "bytes": nbytes,
            "hashed_now": hashed, "unreadable": unreadable, "seconds": round(time.time() - t0, 1),
            "manifest": f"/manifest/{name}.json", "manifest_sha256": hashlib.sha256(data).hexdigest()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roots", default="rootfs,prefix,home,game")
    ap.add_argument("--no-hash", action="store_true")
    a = ap.parse_args()
    idx_p = ASSETS / "manifests" / "index.json"
    try:
        index = json.loads(idx_p.read_text())
    except Exception:
        index = {"roots": {}}
    for name in [r for r in a.roots.split(",") if r]:
        base = ROOTS.get(name)
        if not base or not base.is_dir():
            print(json.dumps({"root": name, "skipped": f"missing dir {base}"}), flush=True)
            continue
        r = scan(name, base, not a.no_hash)
        index["roots"][name] = r
        print(json.dumps(r), flush=True)
    index["server"] = f"http://{CFG.get('LAPTOP_IP', '127.0.0.1')}:{CFG.get('ASSETS_PORT', '8090')}"
    index["generated"] = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    idx_p.parent.mkdir(parents=True, exist_ok=True)
    idx_p.write_text(json.dumps(index, indent=1, sort_keys=True))


if __name__ == "__main__":
    sys.exit(main())
