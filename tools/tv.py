#!/usr/bin/env python3
"""tv.py — LAPTOP's driver for the Apple TV harness.

Reads laptop/config.env (LAPTOP owns it; keys are documented in
laptop/config.env.example). Every command prints exactly one JSON object on
stdout (with --follow: one per batch) and exits non-zero on failure.
Idempotent where it can be: install skips an IPA that is already installed,
launch is a no-op when the app is already up unless --fresh, build only
downloads a release once.

    tv.py build   [--tag build-N] [--wait MIN]         fetch app.ipa of a release
    tv.py install [--tag build-N | --ipa PATH] [--force]
    tv.py jit                                         run JIT_CMD, then re-run the in-app JIT test
    tv.py launch  [--fresh]        tv.py kill        tv.py apps
    tv.py status  | ping | mem | va [--probe] | crash [--clear]
    tv.py shot    [--out PNG]
    tv.py log     [--since N] [--follow] [--out FILE] [--max M]
    tv.py input   (--json J | --key K [--up] | --text S | --mouse X Y [--click B] | --pad BTN [--up])
    tv.py run     [--env K=V ...] [--cwd DIR] -- ARGV...
    tv.py cycle   [--tag build-N | --ipa PATH] [--no-install] [--out DIR]
    tv.py result  NNN-name --verdict PASS|FAIL [--note TEXT] [--from DIR]
"""
import argparse
import datetime as _dt
import hashlib
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

ROOT = pathlib.Path(__file__).resolve().parent.parent
CONFIG = ROOT / "laptop" / "config.env"

# ----------------------------------------------------------------- config


def load_env(path):
    env = {}
    if not path.exists():
        return env
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].strip()
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip()
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
            v = v[1:-1]
        else:
            v = v.split(" #", 1)[0].strip()
        v = os.path.expanduser(os.path.expandvars(v))
        env[k] = v
    return env


CFG = load_env(CONFIG)


def cfg(key, default=None, required=False):
    v = os.environ.get(key) or CFG.get(key)
    if v is None or v == "":
        if required:
            fail(f"{key} is not set in {CONFIG} (see laptop/config.env.example)")
        return default
    return v


def out(obj, code=0):
    print(json.dumps(obj, indent=2, sort_keys=True, default=str))
    sys.stdout.flush()
    sys.exit(code)


def fail(msg, **kw):
    d = {"ok": False, "error": msg}
    d.update(kw)
    out(d, 1)


def now_tag():
    return _dt.datetime.now().strftime("%Y%m%d-%H%M%S")


def out_dir(*parts):
    d = pathlib.Path(cfg("OUT_DIR", str(pathlib.Path.home() / "rltvos" / "out")))
    for p in parts:
        d = d / p
    d.mkdir(parents=True, exist_ok=True)
    return d


STATE_FILE = None


def state_path():
    return out_dir() / "state.json"


def load_state():
    p = state_path()
    if p.exists():
        try:
            return json.loads(p.read_text())
        except Exception:
            return {}
    return {}


def save_state(st):
    state_path().write_text(json.dumps(st, indent=2, sort_keys=True))


# ----------------------------------------------------------------- helpers


def sh(cmd, timeout=None, env=None, input_=None):
    """Run a command (str → shell, list → exec). Returns dict(rc, out, err)."""
    shell = isinstance(cmd, str)
    try:
        p = subprocess.run(cmd, shell=shell, capture_output=True, text=True, timeout=timeout,
                           env={**os.environ, **(env or {})}, input=input_)
        return {"cmd": cmd, "rc": p.returncode, "out": p.stdout, "err": p.stderr}
    except subprocess.TimeoutExpired as e:
        return {"cmd": cmd, "rc": -1, "out": (e.stdout or ""), "err": f"timeout after {timeout}s"}
    except FileNotFoundError as e:
        return {"cmd": cmd, "rc": -2, "out": "", "err": str(e)}


def fmt(template, **kw):
    """Expand {placeholders}; unknown placeholders stay literal."""
    class D(dict):
        def __missing__(self, k):
            return "{" + k + "}"
    return template.format_map(D(**kw))


def tv_ip():
    return cfg("TV_IP", required=True)


def tv_port():
    return int(cfg("TV_PORT", "7777"))


def app_url(path):
    return f"http://{tv_ip()}:{tv_port()}{path}"


def http(method, url, data=None, timeout=10, headers=None, content_type=None):
    h = dict(headers or {})
    body = None
    if data is not None:
        if isinstance(data, (dict, list)):
            body = json.dumps(data).encode()
            h.setdefault("Content-Type", "application/json")
        elif isinstance(data, str):
            body = data.encode()
        else:
            body = data
        if content_type:
            h["Content-Type"] = content_type
    req = urllib.request.Request(url, data=body, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers or {})
    except (urllib.error.URLError, ConnectionError, TimeoutError, OSError) as e:
        return 0, str(e).encode(), {}


def app_get(path, timeout=10):
    return http("GET", app_url(path), timeout=timeout)


def app_json(method, path, data=None, timeout=10):
    st, body, _ = http(method, app_url(path), data=data, timeout=timeout)
    if st == 0:
        return st, {"error": body.decode(errors="replace")}
    try:
        return st, json.loads(body.decode(errors="replace"))
    except Exception:
        return st, {"raw": body.decode(errors="replace")}


def app_is_up(timeout=2):
    st, _, _ = app_get("/ping", timeout=timeout)
    return st == 200


def wait_app(up=True, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if app_is_up(2) == up:
            return True
        time.sleep(1)
    return False


def atvremote_cmd(*args):
    base = [cfg("ATVREMOTE", "atvremote")]
    dev = cfg("TV_DEVICE_ID")
    if dev:
        base += ["--id", dev]
    extra = cfg("ATVREMOTE_ARGS", "")
    if extra:
        base += shlex.split(extra)
    return base + list(args)


def atvremote(*args, timeout=60):
    return sh(atvremote_cmd(*args), timeout=timeout)


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def placeholders(**extra):
    st = load_state()
    d = {
        "tv_ip": cfg("TV_IP", ""),
        "tv_port": str(tv_port()),
        "device_id": cfg("TV_DEVICE_ID", ""),
        "udid": cfg("TV_UDID", ""),
        "bundle_id": st.get("app_id") or cfg("APP_BUNDLE_ID", "dev.rltvos.app"),
        "bundle_prefix": cfg("APP_BUNDLE_ID", "dev.rltvos.app"),
        "apple_id": cfg("APPLE_ID", ""),
        "repo_root": str(ROOT),
    }
    d.update(extra)
    return d


# ----------------------------------------------------------------- github


def gh_api(path, accept="application/vnd.github+json", raw=False, timeout=60):
    repo = cfg("GITHUB_REPO", "user99672223/RLtvOS")
    url = path if path.startswith("http") else f"https://api.github.com/repos/{repo}{path}"
    headers = {"Accept": accept, "User-Agent": "rltvos-tv.py", "X-GitHub-Api-Version": "2022-11-28"}
    tok = cfg("GITHUB_TOKEN")
    if tok:
        headers["Authorization"] = f"Bearer {tok}"
    st, body, hdrs = http("GET", url, timeout=timeout, headers=headers)
    if raw:
        return st, body, hdrs
    try:
        return st, json.loads(body.decode()), hdrs
    except Exception:
        return st, {"raw": body.decode(errors="replace")}, hdrs


def gh_release(tag=None):
    if tag:
        return gh_api(f"/releases/tags/{tag}")
    return gh_api("/releases/latest")


def download_asset(asset, dest):
    """Download a release asset; works for private repos via the API URL."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    st, body, _ = gh_api(asset["url"], accept="application/octet-stream", raw=True, timeout=600)
    if st != 200:
        st, body, _ = http("GET", asset["browser_download_url"], timeout=600,
                           headers={"User-Agent": "rltvos-tv.py"})
    if st != 200:
        return None, f"download failed HTTP {st}: {body[:200]!r}"
    tmp.write_bytes(body)
    tmp.rename(dest)
    return dest, None


def ensure_ipa(tag=None, wait_min=0):
    """Return (tag, ipa_path, info). Downloads if needed; waits for the release if wait_min > 0."""
    ipa_dir = pathlib.Path(cfg("IPA_DIR", str(pathlib.Path.home() / "rltvos" / "ipa")))
    deadline = time.time() + wait_min * 60
    while True:
        st, rel, _ = gh_release(tag)
        assets = {a["name"]: a for a in rel.get("assets", [])} if st == 200 else {}
        if st == 200 and "app.ipa" in assets:
            break
        if time.time() >= deadline:
            fail(f"release {'latest' if not tag else tag} not available (HTTP {st}, assets={list(assets)})",
                 hint="pass --wait MIN to poll, or check the Actions run; private repo needs GITHUB_TOKEN")
        time.sleep(30)
    tag = rel["tag_name"]
    dest = ipa_dir / tag / "app.ipa"
    info = {"tag": tag, "release_url": rel.get("html_url"), "published_at": rel.get("published_at"),
            "body": (rel.get("body") or "")[:400]}
    if not dest.exists() or dest.stat().st_size != assets["app.ipa"]["size"]:
        p, err = download_asset(assets["app.ipa"], dest)
        if err:
            fail(err, tag=tag)
        info["downloaded"] = True
    else:
        info["downloaded"] = False
    if "app.dSYM.zip" in assets:
        d = ipa_dir / tag / "app.dSYM.zip"
        if not d.exists():
            download_asset(assets["app.dSYM.zip"], d)
        info["dsym"] = str(d)
    info["ipa"] = str(dest)
    info["size"] = dest.stat().st_size
    info["sha256"] = sha256_file(dest)
    return tag, dest, info


# ----------------------------------------------------------------- commands


def cmd_build(a):
    tag, ipa, info = ensure_ipa(a.tag, a.wait)
    info["ok"] = True
    out(info)


def resolve_app_id(refresh=False):
    st = load_state()
    if st.get("app_id") and not refresh:
        return st["app_id"]
    prefix = cfg("APP_BUNDLE_ID", "dev.rltvos.app")
    r = atvremote("app_list", timeout=60)
    ids = re.findall(r"\(([A-Za-z0-9_.\-]+)\)", r.get("out", ""))
    match = [i for i in ids if i == prefix or i.startswith(prefix + ".")] or [i for i in ids if "rltvos" in i.lower()]
    if match:
        st["app_id"] = match[0]
        st["app_ids_seen"] = ids
        save_state(st)
        return match[0]
    return None


def cmd_apps(a):
    r = atvremote("app_list", timeout=60)
    ids = re.findall(r"App: ([^()]+) \(([A-Za-z0-9_.\-]+)\)", r.get("out", ""))
    out({"ok": r["rc"] == 0, "apps": [{"name": n.strip(), "id": i} for n, i in ids],
         "app_id": resolve_app_id(refresh=True), "rc": r["rc"], "err": r["err"][-500:]})


def do_install(ipa, tag, force=False):
    ipa = pathlib.Path(ipa)
    if not ipa.exists():
        fail(f"IPA not found: {ipa}")
    digest = sha256_file(ipa)
    st = load_state()
    if st.get("installed_sha256") == digest and not force:
        return {"ok": True, "skipped": True, "reason": "already installed", "tag": st.get("installed_tag"),
                "sha256": digest, "app_id": st.get("app_id")}
    install_cmd = cfg("INSTALL_CMD")
    result = {"tag": tag, "ipa": str(ipa), "sha256": digest}
    if install_cmd:
        cmd = fmt(install_cmd, **placeholders(ipa=str(ipa), tag=tag or ""))
        r = sh(cmd, timeout=int(cfg("INSTALL_TIMEOUT", "900")))
        result.update({"method": "INSTALL_CMD", "cmd": cmd, "rc": r["rc"],
                       "out": r["out"][-2000:], "err": r["err"][-2000:]})
        ok = r["rc"] == 0
    else:
        url = cfg("ATVLOADLY_INSTALL_URL") or (cfg("ATVLOADLY_URL", "http://127.0.0.1:8080").rstrip("/") + "/api/install")
        fields = {}
        try:
            fields = json.loads(cfg("ATVLOADLY_FORM", "{}"))
        except Exception:
            pass
        fields = {k: fmt(str(v), **placeholders(ipa=str(ipa), tag=tag or "")) for k, v in fields.items()}
        file_field = cfg("ATVLOADLY_FILE_FIELD", "file")
        boundary = "----rltvos" + uuid.uuid4().hex
        parts = []
        for k, v in fields.items():
            parts.append(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n".encode())
        parts.append((f"--{boundary}\r\nContent-Disposition: form-data; name=\"{file_field}\"; "
                      f"filename=\"app.ipa\"\r\nContent-Type: application/octet-stream\r\n\r\n").encode())
        parts.append(ipa.read_bytes())
        parts.append(f"\r\n--{boundary}--\r\n".encode())
        body = b"".join(parts)
        st_, resp, _ = http("POST", url, data=body, timeout=int(cfg("INSTALL_TIMEOUT", "900")),
                            content_type=f"multipart/form-data; boundary={boundary}")
        result.update({"method": "atvloadly-http", "url": url, "http": st_, "resp": resp[:2000].decode(errors="replace")})
        ok = 200 <= st_ < 300
        if not ok:
            result["hint"] = ("atvloadly's HTTP API guess failed. Set INSTALL_CMD in laptop/config.env to a command "
                              "that installs {ipa} (e.g. a wrapper around atvloadly's MCP/HTTP API or AltServer), "
                              "or set ATVLOADLY_INSTALL_URL/ATVLOADLY_FORM/ATVLOADLY_FILE_FIELD.")
    result["ok"] = ok
    if ok:
        st.update({"installed_sha256": digest, "installed_tag": tag, "installed_at": now_tag()})
        save_state(st)
        time.sleep(3)
        result["app_id"] = resolve_app_id(refresh=True)
    return result


def cmd_install(a):
    tag = a.tag
    if a.ipa:
        ipa = a.ipa
    else:
        tag, ipa, _ = ensure_ipa(a.tag, a.wait)
    out(do_install(ipa, tag, a.force), 0 if True else 1)


def do_jit(rerun_test=True):
    jit_cmd = cfg("JIT_CMD")
    if not jit_cmd:
        return {"ok": False, "error": "JIT_CMD is not set in laptop/config.env"}
    app_id = resolve_app_id() or cfg("APP_BUNDLE_ID", "dev.rltvos.app")
    pid = ""
    if app_is_up():
        _, s = app_json("GET", "/status")
        pid = str(s.get("pid", ""))
    cmd = fmt(jit_cmd, **placeholders(bundle_id=app_id, pid=pid))
    r = sh(cmd, timeout=int(cfg("JIT_TIMEOUT", "180")))
    res = {"cmd": cmd, "rc": r["rc"], "out": r["out"][-3000:], "err": r["err"][-3000:]}
    settle = float(cfg("JIT_SETTLE_S", "3"))
    time.sleep(settle)
    if rerun_test and wait_app(True, 30):
        st, j = app_json("POST", "/run", {"argv": ["jittest"], "env": [], "cwd": "/"}, timeout=30)
        res["jittest"] = j
        res["ok"] = bool(j.get("ok")) if isinstance(j, dict) else False
    else:
        res["ok"] = r["rc"] == 0
        res["note"] = "app not reachable after JIT_CMD; jit test not re-run"
    return res


def cmd_jit(a):
    r = do_jit(rerun_test=not a.no_test)
    out(r, 0 if r.get("ok") else 1)


def do_launch(fresh=False, timeout=60):
    res = {}
    if app_is_up():
        if not fresh:
            _, s = app_json("GET", "/status")
            return {"ok": True, "already_running": True, "build": s.get("build"), "pid": s.get("pid")}
        app_json("POST", "/kill", {})
        res["killed_previous"] = wait_app(False, 20)
        time.sleep(1)
    launch_cmd = cfg("LAUNCH_CMD")
    app_id = resolve_app_id() or cfg("APP_BUNDLE_ID", "dev.rltvos.app")
    if launch_cmd:
        r = sh(fmt(launch_cmd, **placeholders(bundle_id=app_id)), timeout=120)
    else:
        r = atvremote(f"launch_app={app_id}", timeout=120)
    res.update({"app_id": app_id, "launch_rc": r["rc"], "launch_err": r["err"][-800:], "launch_out": r["out"][-800:]})
    res["up"] = wait_app(True, timeout)
    if res["up"]:
        _, s = app_json("GET", "/status")
        res.update({"build": s.get("build"), "pid": s.get("pid"), "jit": s.get("jit"), "mem": s.get("mem")})
    res["ok"] = res["up"]
    return res


def cmd_launch(a):
    r = do_launch(a.fresh, a.timeout)
    out(r, 0 if r["ok"] else 1)


def cmd_kill(a):
    if not app_is_up():
        out({"ok": True, "already_down": True})
    st, j = app_json("POST", "/kill", {})
    down = wait_app(False, 20)
    kill_cmd = cfg("KILL_CMD")
    if not down and kill_cmd:
        r = sh(fmt(kill_cmd, **placeholders()), timeout=60)
        down = wait_app(False, 20)
        j = {"kill_cmd": r}
    out({"ok": down, "reply": j}, 0 if down else 1)


def cmd_status(a):
    st, j = app_json("GET", "/status")
    out({"ok": st == 200, "http": st, "status": j}, 0 if st == 200 else 1)


def cmd_ping(a):
    up = app_is_up()
    out({"ok": up, "url": app_url("/ping")}, 0 if up else 1)


def cmd_mem(a):
    st, j = app_json("GET", "/mem")
    out({"ok": st == 200, "mem": j}, 0 if st == 200 else 1)


def cmd_va(a):
    path = "/va?probe=1&steps=%d" % a.steps if a.probe else "/va"
    st, j = app_json("GET", path, timeout=120)
    out({"ok": st == 200, "va": j}, 0 if st == 200 else 1)


def cmd_crash(a):
    if a.clear:
        st, j = app_json("DELETE", "/crash")
        out({"ok": st == 200, "reply": j})
    st, body, _ = app_get("/crash")
    if st == 404:
        out({"ok": True, "crash": None, "note": "no crash report"})
    if st != 200:
        fail(f"HTTP {st}", body=body[:300].decode(errors="replace"))
    p = pathlib.Path(a.out) if a.out else out_dir("crashes") / f"crash-{now_tag()}.txt"
    p.write_bytes(body)
    out({"ok": True, "path": str(p), "crash": body.decode(errors="replace")})


def do_shot(path=None):
    st, body, _ = app_get("/screenshot", timeout=30)
    if st != 200:
        return {"ok": False, "http": st, "error": body[:300].decode(errors="replace")}
    p = pathlib.Path(path) if path else out_dir("shots") / f"shot-{now_tag()}.png"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(body)
    return {"ok": True, "path": str(p), "size": len(body)}


def cmd_shot(a):
    r = do_shot(a.out)
    out(r, 0 if r["ok"] else 1)


def do_log(since=None, max_lines=4000, path=None, append=True):
    st_ = load_state()
    if since is None:
        since = int(st_.get("log_next", 1))
    stt, j = app_json("GET", f"/log?since={since}&max={max_lines}", timeout=30)
    if stt != 200:
        return {"ok": False, "http": stt, "error": j}
    lines = j.get("lines", [])
    text = "".join(f"{l['seq']} {l['t']:.1f} {l['s']}\n" for l in lines)
    p = pathlib.Path(path) if path else out_dir("logs") / f"log-{_dt.date.today().isoformat()}.txt"
    p.parent.mkdir(parents=True, exist_ok=True)
    with open(p, "a" if append else "w") as f:
        f.write(text)
    st_["log_next"] = j.get("next", since)
    save_state(st_)
    return {"ok": True, "since": since, "next": j.get("next"), "dropped": j.get("dropped", 0),
            "count": len(lines), "path": str(p), "tail": [l["s"] for l in lines[-15:]]}


def cmd_log(a):
    since = a.since
    if a.all:
        since = 1
    if not a.follow:
        r = do_log(since, a.max, a.out)
        out(r, 0 if r["ok"] else 1)
    nxt = since
    while True:
        r = do_log(nxt, a.max, a.out)
        if r["ok"]:
            nxt = r["next"]
            if r["count"]:
                print(json.dumps(r, sort_keys=True))
                sys.stdout.flush()
        else:
            print(json.dumps(r, sort_keys=True))
            sys.stdout.flush()
        time.sleep(a.interval)


def cmd_input(a):
    events = []
    if a.json:
        events.append(json.loads(a.json))
    if a.key:
        events.append({"type": "key", "key": a.key, "down": not a.up})
    if a.text:
        events.append({"type": "text", "text": a.text})
    if a.mouse:
        ev = {"type": "mouse", "x": int(a.mouse[0]), "y": int(a.mouse[1])}
        if a.click is not None:
            ev["click"] = int(a.click)
        events.append(ev)
    if a.pad:
        events.append({"type": "pad", "button": a.pad, "down": not a.up})
    if not events:
        fail("nothing to send: use --json/--key/--text/--mouse/--pad")
    st, j = app_json("POST", "/input", events if len(events) > 1 else events[0])
    out({"ok": st == 200, "sent": events, "reply": j}, 0 if st == 200 else 1)


def cmd_run(a):
    argv = a.argv
    if argv and argv[0] == "--":
        argv = argv[1:]
    if not argv:
        fail("usage: tv.py run [--env K=V] [--cwd DIR] -- ARGV...")
    st, j = app_json("POST", "/run", {"argv": argv, "env": a.env or [], "cwd": a.cwd}, timeout=a.timeout)
    out({"ok": st == 200 and bool(j.get("ok", True)), "http": st, "reply": j}, 0 if st == 200 else 1)


def cmd_cycle(a):
    d = pathlib.Path(a.out) if a.out else out_dir("cycles", f"cycle-{now_tag()}")
    d.mkdir(parents=True, exist_ok=True)
    report = {"dir": str(d), "steps": {}}

    tag = a.tag
    if not a.no_install:
        if a.ipa:
            ipa = a.ipa
        else:
            tag, ipa, binfo = ensure_ipa(a.tag, a.wait)
            report["steps"]["build"] = binfo
        report["steps"]["install"] = do_install(ipa, tag, a.force)
        if not report["steps"]["install"]["ok"]:
            report["ok"] = False
            (d / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True, default=str))
            out(report, 1)

    if cfg("JIT_LAUNCHES_APP", "0") == "1":
        if app_is_up():
            app_json("POST", "/kill", {})
            wait_app(False, 20)
        report["steps"]["jit"] = do_jit(rerun_test=True)
        report["steps"]["launch"] = {"ok": wait_app(True, 60), "via": "JIT_CMD"}
    else:
        report["steps"]["launch"] = do_launch(fresh=True, timeout=a.timeout)
        if report["steps"]["launch"]["ok"]:
            report["steps"]["jit"] = do_jit(rerun_test=True)

    time.sleep(2)
    report["steps"]["shot"] = do_shot(str(d / "shot.png"))
    st, mem = app_json("GET", "/mem")
    report["steps"]["mem"] = {"ok": st == 200, "mem": mem}
    (d / "mem.json").write_text(json.dumps(mem, indent=2, sort_keys=True))
    st, status = app_json("GET", "/status")
    (d / "status.json").write_text(json.dumps(status, indent=2, sort_keys=True))
    report["steps"]["status"] = {"ok": st == 200, "build": status.get("build"), "jit": status.get("jit"),
                                 "va": status.get("va")}
    report["steps"]["log"] = do_log(1, 8000, str(d / "log.txt"), append=False)
    st, body, _ = app_get("/crash")
    if st == 200:
        (d / "crash.txt").write_bytes(body)
        report["steps"]["crash"] = {"present": True, "path": str(d / "crash.txt")}
    else:
        report["steps"]["crash"] = {"present": False}
    report["ok"] = all(s.get("ok", True) for s in report["steps"].values())
    report["jit_ok"] = bool((status.get("jit") or {}).get("ok"))
    (d / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True, default=str))
    out(report, 0 if report["ok"] else 1)


def cmd_result(a):
    """Assemble handoff/results/<name>/ from a cycle dir (or fresh captures)."""
    rd = ROOT / "handoff" / "results" / a.name
    rd.mkdir(parents=True, exist_ok=True)
    src = pathlib.Path(a.src) if a.src else None
    copied = []
    if src and src.exists():
        for f in src.iterdir():
            if f.is_file():
                (rd / f.name).write_bytes(f.read_bytes())
                copied.append(f.name)
    else:
        r = do_shot(str(rd / "shot.png"))
        if r["ok"]:
            copied.append("shot.png")
        st, mem = app_json("GET", "/mem")
        (rd / "mem.json").write_text(json.dumps(mem, indent=2, sort_keys=True))
        st, status = app_json("GET", "/status")
        (rd / "status.json").write_text(json.dumps(status, indent=2, sort_keys=True))
        do_log(1, 8000, str(rd / "log.txt"), append=False)
        copied += ["mem.json", "status.json", "log.txt"]
    cfgsafe = {k: v for k, v in CFG.items() if not re.search(r"TOKEN|PASS|SECRET|KEY", k)}
    verdict = [f"# {a.name}", "", f"**Verdict: {a.verdict}**", "",
               f"Date: {_dt.datetime.now().isoformat(timespec='seconds')}", "",
               "## What was seen on the TV", "", a.note or "(fill in)", "",
               "## Files", ""] + [f"- {c}" for c in sorted(set(copied))] + [
               "", "## Laptop config (non-secret keys)", "", "```",
               *[f"{k}={v}" for k, v in sorted(cfgsafe.items())], "```", ""]
    (rd / "verdict.md").write_text("\n".join(verdict))
    out({"ok": True, "dir": str(rd), "files": sorted(set(copied)) + ["verdict.md"],
         "next": f"git add handoff/results/{a.name} && git commit -m '[laptop] result {a.name}: {a.verdict}' && git pull --rebase && git push"})


# ----------------------------------------------------------------- main


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)

    p = sp.add_parser("build"); p.add_argument("--tag"); p.add_argument("--wait", type=int, default=0, help="minutes to wait for the release"); p.set_defaults(fn=cmd_build)
    p = sp.add_parser("install"); p.add_argument("--tag"); p.add_argument("--ipa"); p.add_argument("--force", action="store_true"); p.add_argument("--wait", type=int, default=0); p.set_defaults(fn=cmd_install)
    p = sp.add_parser("jit"); p.add_argument("--no-test", action="store_true"); p.set_defaults(fn=cmd_jit)
    p = sp.add_parser("launch"); p.add_argument("--fresh", action="store_true", help="kill first if running"); p.add_argument("--timeout", type=int, default=60); p.set_defaults(fn=cmd_launch)
    p = sp.add_parser("kill"); p.set_defaults(fn=cmd_kill)
    p = sp.add_parser("apps"); p.set_defaults(fn=cmd_apps)
    p = sp.add_parser("status"); p.set_defaults(fn=cmd_status)
    p = sp.add_parser("ping"); p.set_defaults(fn=cmd_ping)
    p = sp.add_parser("mem"); p.set_defaults(fn=cmd_mem)
    p = sp.add_parser("va"); p.add_argument("--probe", action="store_true"); p.add_argument("--steps", type=int, default=1024); p.set_defaults(fn=cmd_va)
    p = sp.add_parser("crash"); p.add_argument("--clear", action="store_true"); p.add_argument("--out"); p.set_defaults(fn=cmd_crash)
    p = sp.add_parser("shot"); p.add_argument("--out"); p.set_defaults(fn=cmd_shot)
    p = sp.add_parser("log"); p.add_argument("--since", type=int); p.add_argument("--all", action="store_true"); p.add_argument("--follow", action="store_true"); p.add_argument("--interval", type=float, default=2.0); p.add_argument("--max", type=int, default=4000); p.add_argument("--out"); p.set_defaults(fn=cmd_log)
    p = sp.add_parser("input"); p.add_argument("--json"); p.add_argument("--key"); p.add_argument("--up", action="store_true"); p.add_argument("--text"); p.add_argument("--mouse", nargs=2); p.add_argument("--click", type=int); p.add_argument("--pad"); p.set_defaults(fn=cmd_input)
    p = sp.add_parser("run"); p.add_argument("--env", action="append"); p.add_argument("--cwd", default="/"); p.add_argument("--timeout", type=int, default=120); p.add_argument("argv", nargs=argparse.REMAINDER); p.set_defaults(fn=cmd_run)
    p = sp.add_parser("cycle"); p.add_argument("--tag"); p.add_argument("--ipa"); p.add_argument("--no-install", action="store_true"); p.add_argument("--force", action="store_true"); p.add_argument("--wait", type=int, default=0); p.add_argument("--timeout", type=int, default=90); p.add_argument("--out"); p.set_defaults(fn=cmd_cycle)
    p = sp.add_parser("result"); p.add_argument("name"); p.add_argument("--verdict", required=True, choices=["PASS", "FAIL"]); p.add_argument("--note"); p.add_argument("--from", dest="src"); p.set_defaults(fn=cmd_result)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
