#!/usr/bin/env python3
"""assets_server.py — serves the guest's file trees to the Apple TV.

Roots (from laptop/config.env): rootfs=ROOTFS_DIR, prefix=WINEPREFIX_DIR,
game=GAME_DIR (or ASSETS_DIR/game), home=HOME_DIR. Read-only.

Endpoints
  GET  /health                      {"ok":true,"roots":{...},"manifest":{...}}
  GET  /manifest.jsonl.gz           gzip; one JSON object per line:
        {"p":"rootfs/usr/bin/ls","t":"f","s":142144,"m":493,"mt":1700000000,"h":"<sha256>"}
        t: f file | d dir | l symlink (+"l":target) | c/b device (+"rdev") | p fifo | s socket
        Directory entries come before their children. "h" is "" when hashing is off.
  GET  /manifest.summary.json       counts, bytes, generation time
  GET|HEAD /f/<root>/<path>         file bytes; honours Range: bytes=a-b | a- | -n (single range);
                                    Accept-Ranges, ETag, Last-Modified, Content-Length
  GET  /stat/<root>/<path>          JSON stat of one entry

Usage
  assets_server.py [--port 8090] [--bind 0.0.0.0] [--no-hash] [--rebuild] [--manifest-only]
The manifest is (re)built at startup when missing or when --rebuild is given;
sha256 results are cached in ASSETS_DIR/.hash-cache.json keyed by (root, path,
size, mtime_ns), so later rebuilds only hash changed files.
"""
import argparse
import concurrent.futures
import gzip
import hashlib
import json
import os
import pathlib
import stat
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent.parent
CONFIG = ROOT / "laptop" / "config.env"


def load_env(path):
    env = {}
    if not path.exists():
        return env
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:]
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
            v = v[1:-1]
        else:
            v = v.split(" #", 1)[0].strip()
        env[k.strip()] = os.path.expanduser(os.path.expandvars(v))
    return env


CFG = load_env(CONFIG)


def cfg(k, d=None):
    return os.environ.get(k) or CFG.get(k) or d


HOME = pathlib.Path.home()
ASSETS_DIR = pathlib.Path(cfg("ASSETS_DIR", str(HOME / "rltvos" / "assets")))
ROOTS = {
    "rootfs": pathlib.Path(cfg("ROOTFS_DIR", str(ASSETS_DIR / "rootfs"))),
    "prefix": pathlib.Path(cfg("WINEPREFIX_DIR", str(ASSETS_DIR / "prefix"))),
    "home": pathlib.Path(cfg("HOME_DIR", str(ASSETS_DIR / "home"))),
    "game": pathlib.Path(cfg("GAME_DIR", str(ASSETS_DIR / "game"))),
}
MANIFEST = ASSETS_DIR / "manifest.jsonl.gz"
SUMMARY = ASSETS_DIR / "manifest.summary.json"
HASH_CACHE = ASSETS_DIR / ".hash-cache.json"

_log_lock = threading.Lock()


def log(*a):
    with _log_lock:
        print(time.strftime("%H:%M:%S"), *a, file=sys.stderr, flush=True)


# ------------------------------------------------------------------ manifest


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb", buffering=0) as f:
        for chunk in iter(lambda: f.read(4 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def walk_root(name, base):
    """Yield (relpath, lstat) depth-first with directories before children."""
    base = base.resolve()
    stack = [base]
    while stack:
        d = stack.pop()
        try:
            entries = sorted(os.scandir(d), key=lambda e: e.name)
        except (PermissionError, FileNotFoundError, NotADirectoryError):
            continue
        subdirs = []
        for e in entries:
            try:
                st = e.stat(follow_symlinks=False)
            except FileNotFoundError:
                continue
            rel = os.path.relpath(e.path, base)
            yield rel, st, e.path
            if stat.S_ISDIR(st.st_mode):
                subdirs.append(pathlib.Path(e.path))
        # push in reverse so traversal order stays sorted
        stack.extend(reversed(subdirs))


def build_manifest(do_hash=True, workers=4):
    t0 = time.time()
    cache = {}
    if HASH_CACHE.exists():
        try:
            cache = json.loads(HASH_CACHE.read_text())
        except Exception:
            cache = {}
    new_cache = {}
    entries = []          # dicts, in order
    to_hash = []          # (index, fullpath)
    counts = {}
    total_bytes = 0
    for name, base in ROOTS.items():
        if not base.exists():
            log(f"root {name} missing at {base}; skipped")
            continue
        n = 0
        entries.append({"p": name, "t": "d", "m": 0o755, "mt": int(base.stat().st_mtime)})
        for rel, st, full in walk_root(name, base):
            p = f"{name}/{rel}"
            mode = stat.S_IMODE(st.st_mode)
            ent = {"p": p, "m": mode, "mt": int(st.st_mtime)}
            if stat.S_ISREG(st.st_mode):
                ent["t"] = "f"
                ent["s"] = st.st_size
                total_bytes += st.st_size
                if do_hash:
                    key = f"{p}|{st.st_size}|{st.st_mtime_ns}"
                    if key in cache:
                        ent["h"] = cache[key]
                        new_cache[key] = cache[key]
                    else:
                        ent["h"] = ""
                        to_hash.append((len(entries), full, key))
                else:
                    ent["h"] = ""
            elif stat.S_ISDIR(st.st_mode):
                ent["t"] = "d"
            elif stat.S_ISLNK(st.st_mode):
                ent["t"] = "l"
                try:
                    ent["l"] = os.readlink(full)
                except OSError:
                    ent["l"] = ""
            elif stat.S_ISCHR(st.st_mode) or stat.S_ISBLK(st.st_mode):
                ent["t"] = "c" if stat.S_ISCHR(st.st_mode) else "b"
                ent["rdev"] = st.st_rdev
            elif stat.S_ISFIFO(st.st_mode):
                ent["t"] = "p"
            elif stat.S_ISSOCK(st.st_mode):
                ent["t"] = "s"
            else:
                continue
            entries.append(ent)
            n += 1
        counts[name] = n
        log(f"scanned {name}: {n} entries")
    if to_hash:
        log(f"hashing {len(to_hash)} files with {workers} workers ...")
        done = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
            futs = {ex.submit(sha256_of, full): (i, key) for i, full, key in to_hash}
            for fut in concurrent.futures.as_completed(futs):
                i, key = futs[fut]
                try:
                    h = fut.result()
                except Exception as e:
                    log(f"hash failed {entries[i]['p']}: {e}")
                    h = ""
                entries[i]["h"] = h
                if h:
                    new_cache[key] = h
                done += 1
                if done % 2000 == 0:
                    log(f"  hashed {done}/{len(to_hash)}")
        HASH_CACHE.write_text(json.dumps(new_cache))
    tmp = MANIFEST.with_suffix(".tmp")
    with gzip.open(tmp, "wt", encoding="utf-8", compresslevel=6) as f:
        for ent in entries:
            f.write(json.dumps(ent, separators=(",", ":"), ensure_ascii=False) + "\n")
    tmp.rename(MANIFEST)
    summary = {"generated": int(time.time()), "seconds": round(time.time() - t0, 1),
               "entries": len(entries), "bytes": total_bytes, "roots": {k: str(v) for k, v in ROOTS.items()},
               "counts": counts, "hashed": do_hash, "manifest_bytes": MANIFEST.stat().st_size}
    SUMMARY.write_text(json.dumps(summary, indent=2))
    log(f"manifest: {len(entries)} entries, {total_bytes / 2**30:.1f} GiB, {summary['manifest_bytes'] / 2**20:.1f} MiB gz, {summary['seconds']} s")
    return summary


# ------------------------------------------------------------------ server


def resolve(root, rel):
    """Map (root, rel) to a real path inside the root, or None."""
    base = ROOTS.get(root)
    if base is None or not base.exists():
        return None
    base_r = base.resolve()
    rel = urllib.parse.unquote(rel)
    if rel.startswith("/") or "\x00" in rel:
        return None
    cand = (base_r / rel).resolve()
    try:
        cand.relative_to(base_r)
    except ValueError:
        return None
    return cand


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "rltvos-assets/1"

    def log_message(self, fmt, *args):
        if os.environ.get("ASSETS_VERBOSE"):
            log(self.address_string(), fmt % args)

    def _json(self, obj, code=200):
        body = json.dumps(obj, indent=2, sort_keys=True).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _err(self, code, msg=""):
        self._json({"ok": False, "error": msg or str(code)}, code)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == "/health":
            summ = json.loads(SUMMARY.read_text()) if SUMMARY.exists() else None
            return self._json({"ok": True, "roots": {k: {"path": str(v), "exists": v.exists()} for k, v in ROOTS.items()},
                               "manifest": summ, "time": int(time.time())})
        if path == "/manifest.jsonl.gz":
            if not MANIFEST.exists():
                return self._err(503, "manifest not built")
            return self._send_file(MANIFEST, "application/gzip")
        if path == "/manifest.summary.json":
            if not SUMMARY.exists():
                return self._err(503, "manifest not built")
            return self._send_file(SUMMARY, "application/json")
        if path.startswith("/f/") or path.startswith("/stat/"):
            kind, rest = path.split("/", 2)[1], path.split("/", 2)[2] if path.count("/") >= 2 else ""
            if "/" not in rest:
                return self._err(400, "expected /<root>/<path>")
            root, rel = rest.split("/", 1)
            full = resolve(root, rel)
            if full is None or not full.exists():
                return self._err(404, "no such file")
            if kind == "stat":
                st = full.lstat()
                return self._json({"ok": True, "size": st.st_size, "mode": stat.S_IMODE(st.st_mode),
                                   "mtime": int(st.st_mtime), "is_dir": full.is_dir(), "is_link": full.is_symlink()})
            if full.is_dir():
                return self._err(400, "is a directory")
            return self._send_file(full, "application/octet-stream")
        return self._err(404, "routes: /health /manifest.jsonl.gz /manifest.summary.json /f/<root>/<path> /stat/<root>/<path>")

    def _send_file(self, full, ctype):
        try:
            f = open(full, "rb")
        except OSError as e:
            return self._err(403, str(e))
        with f:
            st = os.fstat(f.fileno())
            size = st.st_size
            etag = f'"{size:x}-{st.st_mtime_ns:x}"'
            start, end = 0, size - 1
            partial = False
            rng = self.headers.get("Range")
            if rng and rng.startswith("bytes="):
                spec = rng[6:].split(",")[0].strip()
                try:
                    if spec.startswith("-"):
                        n = int(spec[1:])
                        start = max(0, size - n)
                    else:
                        a, _, b = spec.partition("-")
                        start = int(a)
                        end = int(b) if b else size - 1
                    if start >= size or start < 0 or end < start:
                        raise ValueError
                    end = min(end, size - 1)
                    partial = True
                except ValueError:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
            length = end - start + 1 if size else 0
            self.send_response(206 if partial else 200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(length))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("ETag", etag)
            self.send_header("Last-Modified", self.date_time_string(int(st.st_mtime)))
            self.send_header("Cache-Control", "no-cache")
            if partial:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            if self.command == "HEAD" or length == 0:
                return
            f.seek(start)
            remaining = length
            try:
                while remaining > 0:
                    chunk = f.read(min(1 << 20, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=int(cfg("ASSETS_PORT", "8090")))
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--no-hash", action="store_true", help="skip sha256 (faster first start)")
    ap.add_argument("--rebuild", action="store_true", help="rebuild the manifest even if present")
    ap.add_argument("--manifest-only", action="store_true", help="build the manifest and exit")
    ap.add_argument("--workers", type=int, default=4)
    a = ap.parse_args()
    ASSETS_DIR.mkdir(parents=True, exist_ok=True)
    for k, v in ROOTS.items():
        log(f"root {k}: {v} {'ok' if v.exists() else 'MISSING'}")
    if a.rebuild or not MANIFEST.exists():
        build_manifest(do_hash=not a.no_hash, workers=a.workers)
    else:
        log(f"using existing manifest {MANIFEST} (--rebuild to regenerate)")
    if a.manifest_only:
        print(SUMMARY.read_text())
        return
    srv = ThreadingHTTPServer((a.bind, a.port), Handler)
    srv.daemon_threads = True
    log(f"serving on http://{a.bind}:{a.port}/  (health: /health)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
