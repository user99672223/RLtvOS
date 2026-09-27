//! rltvos-jit — LAPTOP's rootless JIT helper for the RLtvOS Apple TV app.
//!
//! tvOS 26+ on TXM hardware (A15 and newer) only executes memory that a debugger
//! has prepared. The app asks for it with StikDebug's universal.js "JIT calls":
//!   JIT26PrepareRegion: mov x16,#1; brk #0xf00d; ret  (x0 = addr|0, x1 = len) -> x0 = addr (0 = fail)
//!   JIT26Detach:        mov x16,#0; brk #0xf00d; ret
//! Transport, no root and no USB: RemotePairing over Wi-Fi with the existing pairing
//! record (atvloadly/PlumeImpactor RpPairingFile), TLS-PSK tunnel, userspace TCP (jktcp),
//! RSD, then CoreDevice appservice (launch suspended) and debugserver (debugproxy).
//!
//!   rltvos-jit probe --tv IP:PORT --pairing FILE
//!   rltvos-jit mount-ddi --tv IP:PORT --pairing FILE --ddi DIR   (tvOS 27+: Cryptex1 DDI)
//!   rltvos-jit run   --tv IP:PORT --pairing FILE (--launch BUNDLE_ID | --pid PID)
//!                    [--timeout SECS] [--status FILE] [--host NAME]
//!
//! stdout: one JSON object per line. `run` exits 0 once the app detached (or exited
//! after at least one prepared region), 1 on any error or timeout. --status FILE is
//! rewritten with the latest state (attached, prepared, detached, exited, error).

use std::{
    net::SocketAddr,
    time::{Duration, Instant},
};

use anyhow::{Context, Result, anyhow, bail};
use idevice::{
    ReadWrite, RsdService,
    core_device::AppServiceClient,
    debug_proxy::{DebugProxyClient, DebugserverCommand},
    remote_pairing::{
        RemotePairingClient, RpPairingFile, RpPairingSocket, connect_tls_psk_tunnel_native,
    },
    rsd::RsdHandshake,
    tcp::handle::AdapterHandle,
};
use tokio::net::TcpStream;

const PAGE: u64 = 16 * 1024;
const BATCH: usize = 128;

// ---------------------------------------------------------------- output

fn js(s: &str) -> String {
    let mut o = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            '\n' => o.push_str("\\n"),
            c if (c as u32) < 0x20 => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

struct Out {
    status: Option<String>,
    t0: Instant,
}

impl Out {
    fn event(&self, state: &str, kv: &[(&str, String)]) {
        let mut parts = vec![
            format!("\"event\":{}", js(state)),
            format!("\"t\":{:.3}", self.t0.elapsed().as_secs_f64()),
        ];
        parts.extend(kv.iter().map(|(k, v)| format!("{}:{}", js(k), v)));
        let line = format!("{{{}}}", parts.join(","));
        println!("{line}");
        if let Some(p) = &self.status {
            let _ = std::fs::write(p, format!("{line}\n"));
        }
    }
}

// ---------------------------------------------------------------- args

struct Args {
    cmd: String,
    tv: SocketAddr,
    pairing: String,
    host: String,
    launch: Option<String>,
    pid: Option<u64>,
    timeout: u64,
    status: Option<String>,
    ddi: Option<String>,
    addr: Option<u64>,
    len: u64,
    packets: Option<String>,
}

fn parse_args() -> Result<Args> {
    let mut it = std::env::args().skip(1);
    let cmd = it.next().ok_or_else(|| anyhow!("usage: rltvos-jit probe|run --tv IP:PORT --pairing FILE ..."))?;
    let (mut tv, mut pairing, mut host) = (None, None, "rltvos-laptop".to_string());
    let (mut launch, mut pid, mut timeout, mut status, mut ddi) = (None, None, 900u64, None, None);
    let (mut addr, mut len, mut packets) = (None, 64u64, None);
    while let Some(a) = it.next() {
        let mut val = || it.next().ok_or_else(|| anyhow!("{a} needs a value"));
        match a.as_str() {
            "--tv" => tv = Some(val()?.parse::<SocketAddr>().context("--tv IP:PORT")?),
            "--pairing" => pairing = Some(val()?),
            "--host" => host = val()?,
            "--launch" => launch = Some(val()?),
            "--pid" => pid = Some(val()?.parse().context("--pid")?),
            "--timeout" => timeout = val()?.parse().context("--timeout")?,
            "--status" => status = Some(val()?),
            "--ddi" => ddi = Some(val()?),
            "--addr" => addr = Some(u64::from_str_radix(val()?.trim_start_matches("0x"), 16).context("--addr HEX")?),
            "--len" => len = val()?.parse().context("--len")?,
            "--packets" => packets = Some(val()?),
            _ => bail!("unknown argument {a}"),
        }
    }
    Ok(Args {
        cmd,
        tv: tv.ok_or_else(|| anyhow!("--tv IP:PORT is required"))?,
        pairing: pairing.ok_or_else(|| anyhow!("--pairing FILE is required"))?,
        host,
        launch,
        pid,
        timeout,
        status,
        ddi,
        addr,
        len,
        packets,
    })
}

// ---------------------------------------------------------------- tunnel

/// Pair-verify with the existing record (never falls back to pair-setup, which
/// would pop a PIN dialog on the TV), then TLS-PSK tunnel + userspace TCP + RSD.
async fn open_tunnel(
    tv: SocketAddr,
    pairing: &str,
    host: &str,
) -> Result<(RemotePairingClient<RpPairingSocket<TcpStream>>, AdapterHandle, RsdHandshake)> {
    let mut rpf = RpPairingFile::read_from_file(pairing)
        .await
        .with_context(|| format!("read pairing file {pairing}"))?;
    let stream = tokio::time::timeout(Duration::from_secs(10), TcpStream::connect(tv))
        .await
        .context("connect to the TV's remotepairing port timed out")?
        .context("connect to the TV's remotepairing port")?;
    let mut rpc = RemotePairingClient::new(RpPairingSocket::new(stream), host);
    rpc.attempt_pair_verify().await.context("pair-verify (start)")?;
    rpc.validate_pairing(&mut rpf)
        .await
        .context("pair-verify rejected: the pairing record is not valid for this TV (re-pair in atvloadly)")?;
    let port = rpc.create_tcp_listener().await.context("create tunnel listener")?;
    let mut taddr = tv;
    taddr.set_port(port);
    let ts = TcpStream::connect(taddr).await.context("connect tunnel port")?;
    let tunnel = connect_tls_psk_tunnel_native(ts, rpc.encryption_key())
        .await
        .context("TLS-PSK tunnel")?;
    let client_ip: std::net::IpAddr = tunnel.info.client_address.parse()?;
    let server_ip: std::net::IpAddr = tunnel.info.server_address.parse()?;
    let mtu = tunnel.info.mtu as usize;
    let rsd_port = tunnel.info.server_rsd_port;
    let raw = tunnel.into_inner();
    let mut adapter = idevice::tcp::adapter::Adapter::new(Box::new(raw), client_ip, server_ip);
    adapter.set_mss(mtu.saturating_sub(60));
    let mut adapter = adapter.to_async_handle();
    let rsd = adapter
        .connect(rsd_port)
        .await
        .map_err(|e| anyhow!("RSD connect: {e}"))?;
    let hs = RsdHandshake::new(rsd).await.context("RSD handshake")?;
    Ok((rpc, adapter, hs))
}

// ---------------------------------------------------------------- gdb-remote helpers

type Dp = DebugProxyClient<Box<dyn ReadWrite>>;

async fn cmd(dp: &mut Dp, c: &str) -> Result<String> {
    Ok(dp
        .send_command(DebugserverCommand::from(c.to_string()))
        .await
        .with_context(|| format!("debugserver command {c}"))?
        .unwrap_or_default())
}

/// Skip inferior-output packets ("O<hex>", but not "OK") until a real reply arrives.
async fn next_reply(dp: &mut Dp, mut r: String) -> Result<String> {
    while r.starts_with('O') && r != "OK" {
        r = dp
            .read_response()
            .await?
            .ok_or_else(|| anyhow!("debugserver closed the connection"))?;
    }
    Ok(r)
}

fn reg(stop: &str, num: &str) -> Option<u64> {
    let key = format!(";{num}:");
    let i = stop.find(&key).map(|i| i + key.len()).or_else(|| {
        stop.find(&format!("{num}:")).filter(|&i| i == 0).map(|i| i + num.len() + 1)
    })?;
    let hex = stop.get(i..i + 16)?;
    let mut v = 0u64;
    for b in (0..8).rev() {
        v = (v << 8) | u64::from_str_radix(hex.get(b * 2..b * 2 + 2)?, 16).ok()?;
    }
    Some(v)
}

fn tid(stop: &str) -> Option<String> {
    let i = stop.find("thread:")? + 7;
    Some(stop[i..].split(';').next()?.to_string())
}

fn le_hex(v: u64) -> String {
    v.to_le_bytes().iter().map(|b| format!("{b:02x}")).collect()
}

fn insn_u32(mem_hex: &str) -> Option<u32> {
    if mem_hex.len() < 8 {
        return None;
    }
    let mut v = 0u32;
    for b in (0..4).rev() {
        v = (v << 8) | u32::from_str_radix(&mem_hex[b * 2..b * 2 + 2], 16).ok()?;
    }
    Some(v)
}

/// Write one byte into every 16 KB page (TXM treats debugger writes as the
/// authorization), pipelined BATCH packets at a time.
async fn bless(dp: &mut Dp, addr: u64, len: u64) -> Result<u64> {
    let pages = len.div_ceil(PAGE);
    let mut done = 0u64;
    while done < pages {
        let n = std::cmp::min(BATCH as u64, pages - done);
        let mut buf = String::new();
        for k in 0..n {
            let body = format!("M{:x},1:69", addr + (done + k) * PAGE);
            let sum = body.bytes().fold(0u8, |a, b| a.wrapping_add(b));
            buf.push_str(&format!("${body}#{sum:02x}"));
        }
        dp.send_raw(buf.as_bytes()).await?;
        for _ in 0..n {
            let r = dp
                .read_response()
                .await?
                .ok_or_else(|| anyhow!("missing reply while preparing pages"))?;
            if r != "OK" {
                bail!("page write at 0x{:x} failed: {r}", addr + done * PAGE);
            }
        }
        done += n;
    }
    Ok(pages)
}

// ---------------------------------------------------------------- commands

async fn probe(a: &Args, out: &Out) -> Result<()> {
    let (_rpc, _adapter, hs) = open_tunnel(a.tv, &a.pairing, &a.host).await?;
    let mut names: Vec<&String> = hs.services.keys().collect();
    names.sort();
    let want = ["com.apple.internal.dt.remote.debugproxy", "com.apple.coredevice.appservice"];
    let present: Vec<String> = want
        .iter()
        .map(|w| format!("{}:{}", js(w), hs.services.contains_key(*w)))
        .collect();
    out.event(
        "probe",
        &[
            ("services", names.len().to_string()),
            ("developer_services", format!("{{{}}}", present.join(","))),
            ("names", format!("[{}]", names.iter().map(|n| js(n)).collect::<Vec<_>>().join(","))),
        ],
    );
    Ok(())
}

/// tvOS/iOS 27+: install the developer disk image as a Cryptex1 through cryptexd
/// (Apple TSS personalization over the laptop's internet). DIR holds the published
/// Cryptex variant: BuildManifest.plist, Image.dmg, Image.dmg.trustcache,
/// Image.dmg.cryptex_info, Image.dmg.root_hash.
async fn mount_ddi(a: &Args, out: &Out) -> Result<()> {
    let dir = std::path::PathBuf::from(a.ddi.as_deref().ok_or_else(|| anyhow!("mount-ddi needs --ddi DIR"))?);
    let rd = |n: &str| std::fs::read(dir.join(n)).with_context(|| format!("read {}", dir.join(n).display()));
    let manifest: plist::Dictionary = plist::from_bytes(&rd("BuildManifest.plist")?).context("parse BuildManifest.plist")?;
    let identity = idevice::tss::select_cryptex_build_identity(&manifest)
        .context("no '... Developer Disk Image Cryptex' identity in BuildManifest.plist")?
        .clone();
    let variant = identity
        .get("Info")
        .and_then(|i| i.as_dictionary())
        .and_then(|i| i.get("Variant"))
        .and_then(|v| v.as_string())
        .unwrap_or("?")
        .to_string();
    let assets = idevice::cryptexd::Cryptex1Assets::from_parts(
        rd("Image.dmg")?,
        rd("Image.dmg.trustcache")?,
        rd("Image.dmg.cryptex_info")?,
        rd("Image.dmg.root_hash")?,
        identity,
    );
    let (_rpc, mut adapter, mut hs) = open_tunnel(a.tv, &a.pairing, &a.host).await?;
    if let Some(c) = idevice::cryptexd::installed_ddi(&mut adapter, &mut hs).await.context("cryptexd: list installed")? {
        out.event("ddi", &[("state", js("already-installed")), ("identifier", js(&c.identifier)), ("version", js(&c.version))]);
        return Ok(());
    }
    out.event("ddi", &[("state", js("installing")), ("variant", js(&variant))]);
    let c = idevice::cryptexd::install_ddi(&mut adapter, &mut hs, &assets)
        .await
        .context("cryptexd: install DDI (TSS personalization + install)")?;
    out.event("ddi", &[("state", js("installed")), ("identifier", js(&c.identifier)), ("version", js(&c.version))]);
    Ok(())
}

async fn run(a: &Args, out: &Out) -> Result<()> {
    let (_rpc, mut adapter, mut hs) = open_tunnel(a.tv, &a.pairing, &a.host).await?;
    out.event("tunnel", &[("services", hs.services.len().to_string())]);

    let pid = match (&a.launch, a.pid) {
        (Some(bundle), _) => {
            let mut apps = AppServiceClient::connect_rsd(&mut adapter, &mut hs)
                .await
                .context("connect com.apple.coredevice.appservice (developer disk image mounted?)")?;
            let r = apps
                .launch_application(bundle.as_str(), &[], true, true, None, None, None)
                .await
                .with_context(|| format!("launch {bundle} suspended"))?;
            out.event("launched", &[("bundle", js(bundle)), ("pid", r.pid.to_string())]);
            r.pid as u64
        }
        (None, Some(p)) => p,
        _ => bail!("run needs --launch BUNDLE_ID or --pid PID"),
    };

    let mut dp: Dp = DebugProxyClient::connect_rsd(&mut adapter, &mut hs)
        .await
        .context("connect com.apple.internal.dt.remote.debugproxy")?;
    dp.send_ack().await?;
    dp.send_ack().await?;
    cmd(&mut dp, "QStartNoAckMode").await?;
    dp.set_ack_mode(false);
    let r = cmd(&mut dp, &format!("vAttach;{pid:x}")).await?;
    if r.starts_with('E') || r.is_empty() {
        bail!("vAttach;{pid:x} failed: {r:?}");
    }
    out.event("attached", &[("pid", pid.to_string()), ("reply", js(&r[..r.len().min(3)]))]);

    let deadline = Instant::now() + Duration::from_secs(a.timeout);
    let (mut regions, mut stops) = (Vec::<(u64, u64)>::new(), 0u64);
    loop {
        if Instant::now() > deadline {
            let _ = cmd(&mut dp, "D").await;
            bail!("timeout after {} s ({} regions prepared)", a.timeout, regions.len());
        }
        let first = cmd(&mut dp, "c").await?;
        let stop = next_reply(&mut dp, first).await?;
        stops += 1;
        if stop.starts_with('W') || stop.starts_with('X') {
            out.event("exited", &[("reply", js(&stop)), ("regions", regions.len().to_string())]);
            return if regions.is_empty() { Err(anyhow!("app exited before preparing any region")) } else { Ok(()) };
        }
        let (Some(t), Some(pc), Some(x16)) = (tid(&stop), reg(&stop, "20"), reg(&stop, "10")) else {
            out.event("unparsed-stop", &[("reply", js(&stop[..stop.len().min(80)]))]);
            continue;
        };
        let insn = insn_u32(&cmd(&mut dp, &format!("m{pc:x},4")).await?).unwrap_or(0);
        let is_brk = insn & 0xFFE0_001F == 0xD420_0000;
        let imm = (insn >> 5) & 0xFFFF;
        if !is_brk || (imm != 0xf00d && imm != 0x69) {
            // Not ours: hand the signal back to the app (its own handlers run).
            let sig = stop.get(1..3).unwrap_or("05").to_string();
            out.event("signal", &[("sig", js(&sig)), ("pc", format!("\"0x{pc:x}\"")), ("insn", format!("\"0x{insn:08x}\""))]);
            let r = cmd(&mut dp, &format!("vCont;S{sig}:{t}")).await?;
            let _ = next_reply(&mut dp, r).await?;
            continue;
        }
        let (x0, x1) = (reg(&stop, "00").unwrap_or(0), reg(&stop, "01").unwrap_or(0));
        cmd(&mut dp, &format!("P20={};thread:{t};", le_hex(pc + 4))).await?;
        if imm == 0x69 {
            // Legacy form: not part of the contract (issue 001/003); fail it like universal.js.
            cmd(&mut dp, &format!("P0={};thread:{t};", le_hex(0xE000_0069))).await?;
            out.event("legacy-0x69", &[("pc", format!("\"0x{pc:x}\""))]);
            continue;
        }
        match x16 {
            0 => {
                let r = cmd(&mut dp, "D").await?;
                out.event("detached", &[("reply", js(&r)), ("regions", regions.len().to_string()), ("stops", stops.to_string())]);
                return Ok(());
            }
            1 => {
                if x0 == 0 && x1 == 0 {
                    continue;
                }
                let t0 = Instant::now();
                let addr = if x0 == 0 {
                    let r = cmd(&mut dp, &format!("_M{x1:x},rx")).await?;
                    match u64::from_str_radix(r.trim(), 16) {
                        Ok(v) if v != 0 => v,
                        _ => {
                            out.event("prepare-failed", &[("len", x1.to_string()), ("reply", js(&r))]);
                            continue; // x0 stays 0 = failure for the app
                        }
                    }
                } else {
                    x0
                };
                let pages = bless(&mut dp, addr, x1).await?;
                cmd(&mut dp, &format!("P0={};thread:{t};", le_hex(addr))).await?;
                regions.push((addr, x1));
                out.event(
                    "prepared",
                    &[
                        ("addr", format!("\"0x{addr:x}\"")),
                        ("len", x1.to_string()),
                        ("pages", pages.to_string()),
                        ("allocated_by", js(if x0 == 0 { "debugger" } else { "app" })),
                        ("ms", t0.elapsed().as_millis().to_string()),
                        ("regions", regions.len().to_string()),
                    ],
                );
            }
            other => out.event("unknown-call", &[("x16", other.to_string())]),
        }
    }
}

/// Read-only diagnostic: attach to PID, read LEN bytes at ADDR (e.g. the JIT code
/// around a fault pc), detach. The app keeps running (vAttach stops it briefly).
async fn peek(a: &Args, out: &Out) -> Result<()> {
    let (pid, addr) = match (a.pid, a.addr) {
        (Some(p), Some(x)) => (p, x),
        _ => bail!("peek needs --pid PID --addr HEX [--len N]"),
    };
    let (_rpc, mut adapter, mut hs) = open_tunnel(a.tv, &a.pairing, &a.host).await?;
    let mut dp: Dp = DebugProxyClient::connect_rsd(&mut adapter, &mut hs)
        .await
        .context("connect com.apple.internal.dt.remote.debugproxy")?;
    dp.send_ack().await?;
    dp.send_ack().await?;
    cmd(&mut dp, "QStartNoAckMode").await?;
    dp.set_ack_mode(false);
    let r = cmd(&mut dp, &format!("vAttach;{pid:x}")).await?;
    if r.starts_with('E') || r.is_empty() {
        bail!("vAttach;{pid:x} failed: {r:?}");
    }
    let mem = cmd(&mut dp, &format!("m{addr:x},{:x}", a.len)).await;
    let d = cmd(&mut dp, "D").await.unwrap_or_default();
    let mem = mem?;
    out.event(
        "peek",
        &[("pid", pid.to_string()), ("addr", format!("\"0x{addr:x}\"")), ("len", a.len.to_string()), ("hex", js(&mem)), ("detach", js(&d))],
    );
    Ok(())
}

/// Read-only diagnostic: attach to PID, send each gdb-remote packet listed in FILE
/// (one per line, e.g. `m1000,4` or `qProcessInfo`), print one JSON line per reply, detach.
async fn gdb(a: &Args, out: &Out) -> Result<()> {
    let (Some(pid), Some(file)) = (a.pid, a.packets.as_ref()) else {
        bail!("gdb needs --pid PID --packets FILE");
    };
    let list = std::fs::read_to_string(file).with_context(|| format!("read {file}"))?;
    let (_rpc, mut adapter, mut hs) = open_tunnel(a.tv, &a.pairing, &a.host).await?;
    let mut dp: Dp = DebugProxyClient::connect_rsd(&mut adapter, &mut hs)
        .await
        .context("connect com.apple.internal.dt.remote.debugproxy")?;
    dp.send_ack().await?;
    dp.send_ack().await?;
    cmd(&mut dp, "QStartNoAckMode").await?;
    dp.set_ack_mode(false);
    let r = cmd(&mut dp, &format!("vAttach;{pid:x}")).await?;
    if r.starts_with('E') || r.is_empty() {
        bail!("vAttach;{pid:x} failed: {r:?}");
    }
    let mut n = 0usize;
    for p in list.lines().map(str::trim).filter(|l| !l.is_empty()) {
        let reply = match cmd(&mut dp, p).await {
            Ok(r) => r,
            Err(e) => format!("ERROR {e:#}"),
        };
        out.event("reply", &[("packet", js(p)), ("reply", js(&reply))]);
        n += 1;
    }
    let d = cmd(&mut dp, "D").await.unwrap_or_default();
    out.event("detached", &[("pid", pid.to_string()), ("packets", n.to_string()), ("reply", js(&d))]);
    Ok(())
}

/// Read-only diagnostic: attach to PID and walk its address space with
/// qMemoryRegionInfo from --addr (default 0x100000000) up to 0x8000000000; one JSON
/// line per mapped region (start, size, permissions, name), then totals; detach.
async fn regions(a: &Args, out: &Out) -> Result<()> {
    let Some(pid) = a.pid else { bail!("regions needs --pid PID") };
    let (_rpc, mut adapter, mut hs) = open_tunnel(a.tv, &a.pairing, &a.host).await?;
    let mut dp: Dp = DebugProxyClient::connect_rsd(&mut adapter, &mut hs)
        .await
        .context("connect com.apple.internal.dt.remote.debugproxy")?;
    dp.send_ack().await?;
    dp.send_ack().await?;
    cmd(&mut dp, "QStartNoAckMode").await?;
    dp.set_ack_mode(false);
    let r = cmd(&mut dp, &format!("vAttach;{pid:x}")).await?;
    if r.starts_with('E') || r.is_empty() {
        bail!("vAttach;{pid:x} failed: {r:?}");
    }
    let (mut addr, end) = (a.addr.unwrap_or(0x1_0000_0000), 0x80_0000_0000u64);
    let (mut mapped, mut n) = (0u64, 0u64);
    let res: Result<()> = async {
        while addr < end && n < 20000 {
            let rep = cmd(&mut dp, &format!("qMemoryRegionInfo:{addr:x}")).await?;
            let field = |k: &str| {
                rep.split(';').find_map(|kv| kv.strip_prefix(k).map(str::to_string))
            };
            let start = field("start:").and_then(|v| u64::from_str_radix(&v, 16).ok()).unwrap_or(addr);
            let size = field("size:").and_then(|v| u64::from_str_radix(&v, 16).ok()).unwrap_or(0);
            if size == 0 {
                out.event("stop", &[("addr", format!("\"0x{addr:x}\"")), ("reply", js(&rep))]);
                break;
            }
            let perms = field("permissions:").unwrap_or_default();
            if !perms.is_empty() {
                mapped += size;
                let name = field("name:").map(|h| {
                    (0..h.len()).step_by(2).filter_map(|i| u8::from_str_radix(h.get(i..i + 2)?, 16).ok()).map(char::from).collect::<String>()
                }).unwrap_or_default();
                out.event("region", &[("start", format!("\"0x{start:x}\"")), ("size", size.to_string()), ("perms", js(&perms)), ("name", js(&name))]);
            }
            n += 1;
            addr = start.saturating_add(size);
        }
        Ok(())
    }.await;
    let d = cmd(&mut dp, "D").await.unwrap_or_default();
    res?;
    out.event("regions-done", &[("queries", n.to_string()), ("mapped_bytes", mapped.to_string()), ("detach", js(&d))]);
    Ok(())
}

/// Send a signal to PID through CoreDevice's appservice (e.g. 9 to kill an app that a
/// dropped debugger session left stopped). `--len` carries the signal number (default 9).
async fn signal(a: &Args, out: &Out) -> Result<()> {
    let Some(pid) = a.pid else { bail!("signal needs --pid PID [--len SIGNAL]") };
    let sig = if a.len == 64 { 9 } else { a.len as u32 };
    let (_rpc, mut adapter, mut hs) = open_tunnel(a.tv, &a.pairing, &a.host).await?;
    let mut apps = AppServiceClient::connect_rsd(&mut adapter, &mut hs)
        .await
        .context("connect com.apple.coredevice.appservice")?;
    let r = apps.send_signal(pid as u32, sig).await.context("sendsignaltoprocess")?;
    out.event("signal-sent", &[("pid", pid.to_string()), ("signal", sig.to_string()), ("reply", js(&format!("{r:?}")))]);
    Ok(())
}

#[tokio::main]
async fn main() {
    let out = Out { status: None, t0: Instant::now() };
    let a = match parse_args() {
        Ok(a) => a,
        Err(e) => {
            out.event("error", &[("error", js(&format!("{e:#}")))]);
            std::process::exit(2);
        }
    };
    let out = Out { status: a.status.clone(), t0: out.t0 };
    let res = match a.cmd.as_str() {
        "probe" => probe(&a, &out).await,
        "run" => run(&a, &out).await,
        "mount-ddi" => mount_ddi(&a, &out).await,
        "peek" => peek(&a, &out).await,
        "gdb" => gdb(&a, &out).await,
        "regions" => regions(&a, &out).await,
        "signal" => signal(&a, &out).await,
        c => Err(anyhow!("unknown command {c} (probe|mount-ddi|run|peek|gdb|regions|signal)")),
    };
    if let Err(e) = res {
        out.event("error", &[("error", js(&format!("{e:#}")))]);
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Shape of a debugserver arm64 stop reply (expedited registers, little-endian hex).
    const STOP: &str = "T05thread:1a2b;threads:1a2b,1a2c;00:0000000000000000;01:0000000800000000;\
10:0100000000000000;1e:48ce0a0001000000;1f:d0c3ffff6f010000;20:a0d20a0001000000;reason:breakpoint;";

    #[test]
    fn parses_stop_reply() {
        assert_eq!(tid(STOP).as_deref(), Some("1a2b"));
        assert_eq!(reg(STOP, "00"), Some(0));
        assert_eq!(reg(STOP, "01"), Some(0x0800_0000)); // 128 MB
        assert_eq!(reg(STOP, "10"), Some(1)); // x16 = PrepareRegion
        assert_eq!(reg(STOP, "20"), Some(0x1_000a_d2a0)); // pc
        assert_eq!(reg(STOP, "02"), None);
    }

    #[test]
    fn encodes_little_endian() {
        assert_eq!(le_hex(0x1_000a_d2a4), "a4d20a0001000000");
        assert_eq!(le_hex(0xE000_0069), "690000e000000000");
    }

    #[test]
    fn recognises_brk_f00d() {
        // brk #0xf00d = 0xD43E01A0, stored little-endian in memory as "a0013ed4".
        let insn = insn_u32("a0013ed4").unwrap();
        assert_eq!(insn, 0xD43E_01A0);
        assert_eq!(insn & 0xFFE0_001F, 0xD420_0000);
        assert_eq!((insn >> 5) & 0xFFFF, 0xf00d);
        assert_eq!(insn_u32("1f2003d5").map(|i| i & 0xFFE0_001F == 0xD420_0000), Some(false)); // nop
    }
}
