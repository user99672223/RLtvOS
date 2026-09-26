#!/usr/bin/env python3
"""atvloadly.py — LAPTOP's client for the atvloadly container (v0.4.8).

Uses the MCP endpoint (streamable HTTP) for install/refresh/status and the web
UI's HTTP API for whole-screen screenshots and DDI mounting. Reads
laptop/config.env. Prints one JSON object; exit code 0 on success.

    atvloadly.py install IPA [--timeout S]   sideload IPA (must be under IPA_DIR, else it is copied there)
    atvloadly.py apps                        installed apps + refresh state
    atvloadly.py refresh APP_ID [--timeout S]
    atvloadly.py shot OUT.jpg                full-screen TV screenshot (RSD, via plumesign)
    atvloadly.py mount                       mount the personalized developer disk image
    atvloadly.py tools                       list MCP tools

IPA_DIR on the host is bind-mounted read-only at ATVLOADLY_SHARE_CONTAINER
(/share/ipa) in the container, so install passes a container path, not a URL.
"""
import argparse
import base64
import calendar
import hashlib
import json
import os
import pathlib
import plistlib
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile

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


def cfg(k, d=None):
    return os.environ.get(k) or CFG.get(k) or d


def out(obj, code=0):
    print(json.dumps(obj, indent=2, sort_keys=True, default=str))
    sys.exit(code)


class MCP:
    def __init__(self, url):
        self.url = url
        self.sid = None
        self.n = 0
        r, hdrs = self._post({"jsonrpc": "2.0", "id": self._id(), "method": "initialize",
                              "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                                         "clientInfo": {"name": "rltvos-laptop", "version": "1"}}})
        self.sid = hdrs.get("Mcp-Session-Id") or hdrs.get("mcp-session-id")
        self._post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _id(self):
        self.n += 1
        return self.n

    def _post(self, msg, timeout=60):
        h = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        if self.sid:
            h["Mcp-Session-Id"] = self.sid
        req = urllib.request.Request(self.url, data=json.dumps(msg).encode(), headers=h, method="POST")
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body = r.read().decode(errors="replace")
            hdrs = dict(r.headers)
        data = None
        for line in body.splitlines():
            if line.startswith("data: "):
                data = json.loads(line[6:])
        if data is None and body.strip().startswith("{"):
            data = json.loads(body)
        return data, hdrs

    def call(self, name, args=None):
        r, _ = self._post({"jsonrpc": "2.0", "id": self._id(), "method": "tools/call",
                           "params": {"name": name, "arguments": args or {}}})
        if r is None:
            raise RuntimeError(f"no response for {name}")
        if "error" in r:
            raise RuntimeError(f"{name}: {r['error']}")
        res = r.get("result", {})
        text = "".join(c.get("text", "") for c in res.get("content", []) if c.get("type") == "text")
        if res.get("isError"):
            raise RuntimeError(f"{name}: {text}")
        sc = res.get("structuredContent")
        if sc is not None:
            return sc
        try:
            return json.loads(text)
        except Exception:
            return {"text": text}

    def tools(self):
        r, _ = self._post({"jsonrpc": "2.0", "id": self._id(), "method": "tools/list"})
        return [t["name"] for t in r["result"]["tools"]]


def mcp():
    return MCP(cfg("ATVLOADLY_MCP", "http://127.0.0.1:5533/mcp"))


def http_json(method, path, timeout=120):
    url = cfg("ATVLOADLY_URL", "http://127.0.0.1:5533").rstrip("/") + path
    req = urllib.request.Request(url, method=method, data=b"" if method == "POST" else None)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def ipa_info(p):
    with zipfile.ZipFile(p) as z:
        name = next(n for n in z.namelist() if re.match(r"^Payload/[^/]+\.app/Info\.plist$", n))
        info = plistlib.loads(z.read(name))
    return {"bundle_id": info.get("CFBundleIdentifier"), "version": info.get("CFBundleVersion"),
            "short_version": info.get("CFBundleShortVersionString"), "name": info.get("CFBundleName")}


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def container_path(ipa):
    ipa = pathlib.Path(ipa).resolve()
    ipa_dir = pathlib.Path(cfg("IPA_DIR", str(pathlib.Path.home() / "rltvos" / "ipa"))).resolve()
    try:
        rel = ipa.relative_to(ipa_dir)
    except ValueError:
        dst = ipa_dir / "manual" / f"{sha256(ipa)[:12]}-{ipa.name}"
        dst.parent.mkdir(parents=True, exist_ok=True)
        if not dst.exists():
            shutil.copy2(ipa, dst)
        rel = dst.relative_to(ipa_dir)
    if not str(rel).lower().endswith(".ipa"):
        raise SystemExit("IPA path must end in .ipa")
    return str(pathlib.PurePosixPath(cfg("ATVLOADLY_SHARE_CONTAINER", "/share/ipa")) / rel)


def pick_account(m):
    want = cfg("ATVLOADLY_ACCOUNT_ID")
    accs = m.call("get_account_list").get("available_accounts", [])
    if want:
        return want
    valid = [a for a in accs if a.get("status") == "valid"] or accs
    if not valid:
        raise RuntimeError("atvloadly has no Apple account")
    return valid[0]["account_id"]


def pick_device(m):
    want = cfg("ATVLOADLY_DEVICE_ID")
    devs = m.call("get_device_list").get("available_devices", [])
    udid = cfg("TV_UDID")
    for d in devs:
        if d.get("id") == want or (udid and d.get("udid") == udid):
            return d["id"]
    if len(devs) == 1:
        return devs[0]["id"]
    raise RuntimeError(f"device not found among {devs}")


def refresh_items(m):
    return m.call("get_refresh_status").get("items", [])


def wait_install_idle(m, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not m.call("get_install_status").get("install_in_progress"):
            return True
        time.sleep(3)
    return False


def cmd_install(a):
    ipa = pathlib.Path(a.ipa)
    if not ipa.exists():
        out({"ok": False, "error": f"no such file: {ipa}"}, 1)
    info = ipa_info(ipa)
    cpath = container_path(ipa)
    chk = subprocess.run(["docker", "exec", "atvloadly", "test", "-r", cpath], capture_output=True)
    if chk.returncode != 0:
        out({"ok": False, "error": f"container cannot read {cpath} (is IPA_DIR mounted at /share/ipa?)"}, 1)
    m = mcp()
    res = {"ipa": str(ipa), "container_path": cpath, "sha256": sha256(ipa), **info}
    if not wait_install_idle(m, a.timeout):
        out({**res, "ok": False, "error": "another install is still in progress"}, 1)
    t0 = time.time()
    q = m.call("install_app", {"ipa_url": cpath, "device_id": pick_device(m), "account_id": pick_account(m)})
    q.pop("selected_account", None)  # keep account identifiers out of result files (public repo)
    res["queued"] = q
    if q.get("status") != "installing":
        out({**res, "ok": False, "error": "install not queued"}, 1)
    time.sleep(5)
    wait_install_idle(m, a.timeout)
    # Success = atvloadly's app record for this bundle id got a successful refresh after t0.
    item = None
    for _ in range(10):
        for it in refresh_items(m):
            if it.get("bundle_identifier") == info["bundle_id"]:
                item = it
        if item and item.get("last_refresh_at"):
            break
        time.sleep(2)
    res["atvloadly_app"] = item
    ok = False
    if item and item.get("last_success"):
        ts = item.get("last_refresh_at", "")
        try:
            t = calendar.timegm(time.strptime(ts[:19], "%Y-%m-%dT%H:%M:%S"))
            ok = t >= t0 - 5
        except Exception:
            ok = True
    if not ok:
        logtail = subprocess.run(["docker", "exec", "atvloadly", "tail", "-15", "/data/app.log"],
                                 capture_output=True, text=True).stdout
        res["log_tail"] = logtail.splitlines()
    res["ok"] = ok
    res["seconds"] = round(time.time() - t0, 1)
    out(res, 0 if ok else 1)


def cmd_apps(a):
    m = mcp()
    out({"ok": True, "apps": m.call("get_app_list").get("items", []), "refresh": refresh_items(m)})


def cmd_refresh(a):
    m = mcp()
    q = m.call("refresh_app", {"app_id": a.app_id})
    t0 = time.time()
    while time.time() - t0 < a.timeout:
        time.sleep(3)
        st = m.call("get_refresh_status", {"app_id": a.app_id}).get("items", [{}])[0]
        if st.get("refresh_state") in ("completed_success", "completed_failed"):
            out({"ok": st["refresh_state"] == "completed_success", "queued": q, "status": st},
                0 if st["refresh_state"] == "completed_success" else 1)
    out({"ok": False, "error": "timeout", "queued": q}, 1)


def cmd_shot(a):
    dev = cfg("ATVLOADLY_DEVICE_ID")
    r = http_json("POST", f"/api/devices/{dev}/screenshot", timeout=90)
    data = (r.get("data") or {}).get("data") if isinstance(r.get("data"), dict) else None
    if not data:
        out({"ok": False, "error": r}, 1)
    data = re.sub(r"^data:image/[a-z]+;base64,", "", data)
    p = pathlib.Path(a.out)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(base64.b64decode(data))
    out({"ok": True, "path": str(p), "size": p.stat().st_size})


def cmd_mount(a):
    dev = cfg("ATVLOADLY_DEVICE_ID")
    r = http_json("POST", f"/api/devices/{dev}/mountimage", timeout=180)
    out({"ok": r.get("status") in (True, "success", 200) or r.get("data") == "success", "reply": r})


def cmd_tools(a):
    out({"ok": True, "tools": mcp().tools()})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("install"); p.add_argument("ipa"); p.add_argument("--timeout", type=int, default=int(cfg("INSTALL_TIMEOUT", "900"))); p.set_defaults(fn=cmd_install)
    p = sp.add_parser("apps"); p.set_defaults(fn=cmd_apps)
    p = sp.add_parser("refresh"); p.add_argument("app_id", type=int); p.add_argument("--timeout", type=int, default=300); p.set_defaults(fn=cmd_refresh)
    p = sp.add_parser("shot"); p.add_argument("out"); p.set_defaults(fn=cmd_shot)
    p = sp.add_parser("mount"); p.set_defaults(fn=cmd_mount)
    p = sp.add_parser("tools"); p.set_defaults(fn=cmd_tools)
    a = ap.parse_args()
    try:
        a.fn(a)
    except (RuntimeError, urllib.error.URLError, OSError, KeyError) as e:
        out({"ok": False, "error": f"{type(e).__name__}: {e}"}, 1)


if __name__ == "__main__":
    main()
