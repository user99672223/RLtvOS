import Foundation
import UIKit

/// Process-wide state: probe results, input queue, status JSON and the lines
/// drawn on the Metal console. Phase A has no guest kernel behind it yet.
final class HostState {
    static let shared = HostState()

    let startDate = Date()
    let workQueue = DispatchQueue(label: "rl.host.work", qos: .userInitiated)
    private let lock = NSLock()

    private var jit: [String: Any] = ["ok": false, "stage": "pending"]
    private var va: [String: Any] = ["stage": "pending"]
    private var sysinfo: [String: Any] = [:]
    private var inputQueue: [[String: Any]] = []
    weak var renderer: ConsoleRenderer?

    var buildTag: String {
        (Bundle.main.object(forInfoDictionaryKey: "RLBuildTag") as? String) ?? "dev"
    }

    var cachesDir: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
    }

    var crashFile: URL { cachesDir.appendingPathComponent("crash-last.txt") }

    // MARK: - lifecycle

    func bootstrap() {
        try? FileManager.default.createDirectory(at: cachesDir, withIntermediateDirectories: true)
        let rc = rl_crash_install(crashFile.path)
        rl_log_str("host: RLtvOS \(buildTag) pid \(getpid()) tvOS \(UIDevice.current.systemVersion) crash-handler rc=\(rc)")
        if let prev = try? String(contentsOf: crashFile, encoding: .utf8), !prev.isEmpty {
            let first = prev.split(separator: "\n").first.map(String.init) ?? ""
            rl_log_str("host: previous crash report present: \(first)")
        }
        let info = HostState.parseJSON(HostState.fill(4096) { rl_sysinfo_json($0, 4096) })
        lock.lock(); sysinfo = info; lock.unlock()
        rl_log_str("host: sysinfo \(HostState.compact(info))")
        // A previous run that died inside a trusted jittest left the marker.
        if let m = try? String(contentsOf: jitMarkerFile, encoding: .utf8), !m.isEmpty {
            lock.lock(); lastJitKill = m; lock.unlock()
            rl_log_str("host: previous run was killed during jittest: \(m) (unauthorized page → SIGKILL?)")
            try? FileManager.default.removeItem(at: jitMarkerFile)
        }
        // JIT arena: fixed region the debugger authorizes page by page.
        let arenaRC = rl_jit_arena_init(Int(jitArenaMB) << 20)
        rl_log_str("host: jit arena init rc=\(arenaRC) \(HostState.fill(512) { rl_jit_arena_json($0, 512) })")
        workQueue.async { self.runProbes() }
    }

    var jitArenaMB: Int {
        (Bundle.main.object(forInfoDictionaryKey: "RLJitArenaMB") as? NSNumber)?.intValue
            ?? Int((Bundle.main.object(forInfoDictionaryKey: "RLJitArenaMB") as? String) ?? "") ?? 64
    }

    var jitMarkerFile: URL { cachesDir.appendingPathComponent("jittest-inflight.txt") }
    private var lastJitKill: String = ""

    func jitArenaInfo() -> [String: Any] {
        HostState.parseJSON(HostState.fill(512) { rl_jit_arena_json($0, 512) })
    }

    /// Executes code in the arena (or a fresh page). Writes a marker first so
    /// a SIGKILL (unauthorized page under TXM) is visible after relaunch.
    @discardableResult
    func runJitExecTest(page: Int32, madvise: Bool, fresh: Bool) -> [String: Any] {
        let marker = "page=\(page) madvise=\(madvise) fresh=\(fresh) at=\(Date())\n"
        try? marker.write(to: jitMarkerFile, atomically: true, encoding: .utf8)
        let s = HostState.fill(2048) { _ = rl_jit_exec_test_json(page, madvise ? 1 : 0, fresh ? 1 : 0, $0, 2048) }
        try? FileManager.default.removeItem(at: jitMarkerFile)
        var d = HostState.parseJSON(s)
        d["trusted"] = true
        lock.lock(); jit = d; lock.unlock()
        rl_log_str("host: jit exec test \(s)")
        return d
    }

    private func runProbes() {
        _ = runJitTest()
        _ = runVaProbe()
        // C++ core (CMake-built static library) runtime self-test.
        rlcore_set_log { line in if let line = line { rl_log_str(String(cString: line)) } }
        rl_log_str("host: rlcore \(String(cString: rlcore_version()))")
        let c = HostState.fill(1024) { _ = rlcore_selftest($0, 1024) }
        let cd = HostState.parseJSON(c)
        lock.lock(); core = cd; lock.unlock()
        rl_log_str("host: rlcore selftest \(c)")
        let m = memInfo()
        rl_log_str("host: mem \(HostState.compact(m))")
    }

    private var core: [String: Any] = ["stage": "pending"]
    func coreInfo() -> [String: Any] { lock.lock(); defer { lock.unlock() }; return core }
    func vfsStats() -> [String: Any] { HostState.parseJSON(HostState.fill(2048) { rlcore_vfs_stats($0, 2048) }) }

    /// Map-only probe (never executes): reports whether RWX mapping works.
    @discardableResult
    func runJitTest() -> [String: Any] {
        let s = HostState.fill(2048) { _ = rl_jit_test_json(0, $0, 2048) }
        let d = HostState.parseJSON(s)
        lock.lock(); jit = d; lock.unlock()
        rl_log_str("host: jit map probe \(s)")
        return d
    }

    @discardableResult
    func runVaProbe(stepLimitGB: Int32 = 1024) -> [String: Any] {
        let s = HostState.fill(1024) { rl_va_probe_json($0, 1024, stepLimitGB) }
        let d = HostState.parseJSON(s)
        lock.lock(); va = d; lock.unlock()
        rl_log_str("host: va \(s)")
        return d
    }

    // MARK: - accessors

    func memInfo() -> [String: Any] {
        HostState.parseJSON(HostState.fill(1024) { rl_mem_json($0, 1024) })
    }

    func jitInfo() -> [String: Any] { lock.lock(); defer { lock.unlock() }; return jit }
    func vaInfo() -> [String: Any] { lock.lock(); defer { lock.unlock() }; return va }
    func sysInfo() -> [String: Any] { lock.lock(); defer { lock.unlock() }; return sysinfo }

    func enqueueInput(_ ev: [String: Any]) -> Int {
        lock.lock(); defer { lock.unlock() }
        inputQueue.append(ev)
        if inputQueue.count > 4096 { inputQueue.removeFirst(inputQueue.count - 4096) }
        return inputQueue.count
    }

    func drainInput() -> [[String: Any]] {
        lock.lock(); defer { lock.unlock() }
        let q = inputQueue
        inputQueue.removeAll()
        return q
    }

    func status() -> [String: Any] {
        let pi = ProcessInfo.processInfo
        return [
            "app": "RLtvOS",
            "build": buildTag,
            "pid": Int(getpid()),
            "uptime_s": Date().timeIntervalSince(startDate),
            "tvos": UIDevice.current.systemVersion,
            "device": UIDevice.current.model,
            "thermal": HostState.thermalName(pi.thermalState),
            "processors": pi.processorCount,
            "active_processors": pi.activeProcessorCount,
            "physical_memory": pi.physicalMemory,
            "low_power": pi.isLowPowerModeEnabled,
            "sysinfo": sysInfo(),
            "jit": jitInfo(),
            "jit_arena": jitArenaInfo(),
            "last_jit_kill": { lock.lock(); defer { lock.unlock() }; return lastJitKill }(),
            "core": coreInfo(),
            "vfs": vfsStats(),
            "va": vaInfo(),
            "mem": memInfo(),
            "frames": renderer?.frameCount ?? 0,
            "requests": httpd_request_count(),
            "log_next": rl_log_next_seq(),
            "guest": ["kernel": "none (phase A)", "processes": [] as [Any]],
            "input_queued": inputQueue.count,
            "caches_dir": cachesDir.path,
            "crash_report_present": FileManager.default.fileExists(atPath: crashFile.path),
        ]
    }

    /// POST /run — phase A: built-in host commands only.
    func run(argv: [String], env: [String], cwd: String) -> [String: Any] {
        guard let cmd = argv.first else { return ["ok": false, "error": "empty argv"] }
        switch cmd {
        case "jittest":
            // jittest [--trust|--force] [--page N] [--madvise] [--fresh]
            // Without --trust only the map probe runs (safe). With --trust the
            // code is executed: tv.py jit sets it after the debugger authorized
            // the arena pages. --fresh executes in a fresh, unauthorized page
            // (expected to be SIGKILLed on tvOS 26+; run it last).
            let flags = Array(argv.dropFirst())
            let trust = flags.contains("--trust") || flags.contains("--force")
            var page: Int32 = 0
            if let i = flags.firstIndex(of: "--page"), i + 1 < flags.count { page = Int32(flags[i + 1]) ?? 0 }
            let madv = flags.contains("--madvise")
            let fresh = flags.contains("--fresh")
            if !trust {
                let d = runJitTest()
                return ["ok": false, "executed": false, "jit": d, "arena": jitArenaInfo(),
                        "hint": "authorize the arena with a debugger write per page, then jittest --trust"]
            }
            let d = runJitExecTest(page: page, madvise: madv, fresh: fresh)
            return ["ok": (d["ok"] as? Bool) ?? false, "executed": true, "jit": d, "arena": jitArenaInfo()]
        case "vaprobe":
            let limit = argv.count > 1 ? (Int32(argv[1]) ?? 1024) : 1024
            return ["ok": true, "va": runVaProbe(stepLimitGB: limit)]
        case "memprobe":
            return ["ok": true, "mem": memInfo()]
        case "crashtest":
            let kind = argv.count > 1 ? (Int32(argv[1]) ?? 0) : 0
            DispatchQueue.global().asyncAfter(deadline: .now() + 0.3) { rl_crash_now(kind) }
            return ["ok": true, "note": "crashing in 300 ms (kind \(kind)); relaunch and GET /crash"]
        case "log":
            rl_log_str("run: " + argv.dropFirst().joined(separator: " "))
            return ["ok": true]
        case "vfs-mount":
            // vfs-mount http://LAPTOP_IP:8090 [cache-subdir]
            guard argv.count > 1 else { return ["ok": false, "error": "usage: vfs-mount http://host:port"] }
            let sub = argv.count > 2 ? argv[2] : "vfs-blocks"
            let dir = cachesDir.appendingPathComponent(sub).path
            let s = HostState.fill(8192) { _ = rlcore_vfs_mount(argv[1], dir, $0, 8192) }
            rl_log_str("vfs: mount \(argv[1]) → \(s.prefix(300))")
            return HostState.parseJSON(s)
        case "vfs-stat":
            guard argv.count > 1 else { return ["ok": false, "error": "usage: vfs-stat PATH [nofollow]"] }
            let follow: Int32 = argv.count > 2 && argv[2] == "nofollow" ? 0 : 1
            return HostState.parseJSON(HostState.fill(8192) { _ = rlcore_vfs_stat(argv[1], follow, $0, 8192) })
        case "vfs-ls":
            let path = argv.count > 1 ? argv[1] : "/"
            return HostState.parseJSON(HostState.fill(262144) { _ = rlcore_vfs_ls(path, $0, 262144) })
        case "vfs-cat":
            // vfs-cat PATH [off] [len]
            guard argv.count > 1 else { return ["ok": false, "error": "usage: vfs-cat PATH [off] [len]"] }
            let off = argv.count > 2 ? (UInt64(argv[2]) ?? 0) : 0
            let len = argv.count > 3 ? (UInt32(argv[3]) ?? 4096) : 4096
            let s = HostState.fill(262144) { _ = rlcore_vfs_read(argv[1], off, len, $0, 262144) }
            var d = HostState.parseJSON(s)
            if let t = d["text"] as? String {
                for line in t.split(separator: "\n").prefix(20) { rl_log_str("guest-file: \(line)") }
            }
            d["stats"] = vfsStats()
            return d
        case "sleep":
            let s = argv.count > 1 ? (Double(argv[1]) ?? 1) : 1
            Thread.sleep(forTimeInterval: min(s, 30))
            return ["ok": true, "slept": min(s, 30)]
        default:
            return [
                "ok": false,
                "error": "no guest kernel yet (phase A)",
                "builtins": ["jittest [--trust] [--page N] [--madvise] [--fresh]", "vaprobe [steps]", "memprobe",
                             "crashtest [0|1|2]", "log ...", "sleep s", "vfs-mount http://host:port [cache-subdir]",
                             "vfs-stat PATH [nofollow]", "vfs-ls PATH", "vfs-cat PATH [off] [len]"],
                "argv": argv, "env": env, "cwd": cwd,
            ]
        }
    }

    // MARK: - console

    func consoleLines() -> [String] {
        let mem = memInfo()
        let j = jitInfo()
        let v = vaInfo()
        let si = sysInfo()
        var lines: [String] = []
        lines.append("RLtvOS \(buildTag)   pid \(getpid())   tvOS \(UIDevice.current.systemVersion)   \(si["hw_machine"] ?? "?")   \(si["kern_osversion"] ?? "?")")
        lines.append("up \(Int(Date().timeIntervalSince(startDate))) s   frames \(renderer?.frameCount ?? 0)   http requests \(httpd_request_count())   port 7777")
        lines.append("")
        let fp = HostState.dbl(mem["phys_footprint_mb"])
        let peak = HostState.dbl(mem["peak_phys_footprint"]) / 1048576.0
        let avail = HostState.dbl(mem["available_mb"])
        let limit = HostState.dbl(mem["limit_estimate"]) / 1048576.0
        lines.append(String(format: "MEM  phys_footprint %.1f MB   peak %.1f MB   available %.1f MB   limit~ %.0f MB", fp, peak, avail, limit))
        if let stage = v["stage"] as? String {
            lines.append("VA   \(stage)")
        } else {
            lines.append("VA   max contiguous \(v["max_contiguous_gb"] ?? "?") GB   1 GB steps \(v["total_1gb_steps"] ?? "?") / \(v["step_limit_gb"] ?? "?")   range \(v["lowest"] ?? "?")-\(v["highest"] ?? "?")")
        }
        let arena = jitArenaInfo()
        lines.append("ARENA \(arena["prot"] ?? "?") \(arena["size_mb"] ?? "?") MB at \(arena["base"] ?? "?")  (\(arena["pages"] ?? "?") pages of 16 KB; rwx_errno \(arena["rwx_errno"] ?? "?"))")
        if (j["ok"] as? Bool) == true {
            lines.append("JIT ok   \(j["where"] ?? j["method"] ?? "?") \(j["prot"] ?? ""): wrote+executed -> \(j["result1"] ?? "?"), rewrote+executed -> \(j["result2"] ?? "?")")
        } else if let stage = j["stage"] as? String {
            lines.append("JIT  \(stage) rwx_errno=\(j["rwx_errno"] ?? "?") ptraced=\(j["ptraced"] ?? "?") cs_debugged=\(j["cs_debugged"] ?? "?")  (awaiting tv.py jit)")
        } else {
            lines.append("JIT FAIL \(HostState.compact(j))")
        }
        let kill: String = { lock.lock(); defer { lock.unlock() }; return lastJitKill }()
        if !kill.isEmpty {
            lines.append("JIT  previous run KILLED during jittest: \(kill.trimmingCharacters(in: .whitespacesAndNewlines))")
        }
        lines.append("DBG  ptraced=\(si["ptraced"] ?? "?")  cs_debugged=\(j["cs_debugged"] ?? si["cs_debugged"] ?? "?")  sigaltstack=\(si["sigaltstack"] ?? "?")  crash_report=\(FileManager.default.fileExists(atPath: crashFile.path) ? "yes" : "no")")
        let c = coreInfo()
        lines.append("CORE \((c["ok"] as? Bool) == true ? "ok" : (c["stage"] as? String ?? "FAIL"))  \(c["version"] ?? "")  threads=\(c["threads"] ?? "?") exceptions=\(c["exceptions"] ?? "?") alloc64mb=\(c["alloc64mb"] ?? "?")")
        lines.append("")
        lines.append("--- log tail ---")
        let next = rl_log_next_seq()
        let from = next > 14 ? next - 14 : 1
        if let c = rl_log_text_since(from, 14, nil) {
            let text = String(cString: c)
            free(c)
            for l in text.split(separator: "\n") { lines.append(String(l)) }
        }
        return lines
    }

    // MARK: - helpers

    static func fill(_ cap: Int, _ body: (UnsafeMutablePointer<CChar>) -> Void) -> String {
        var buf = [CChar](repeating: 0, count: cap)
        buf.withUnsafeMutableBufferPointer { p in body(p.baseAddress!) }
        return buf.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
    }

    static func parseJSON(_ s: String) -> [String: Any] {
        guard let d = s.data(using: .utf8),
              let o = try? JSONSerialization.jsonObject(with: d, options: []),
              let dict = o as? [String: Any] else {
            return ["parse_error": s]
        }
        return dict
    }

    static func compact(_ o: Any) -> String {
        guard JSONSerialization.isValidJSONObject(o),
              let d = try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys]),
              let s = String(data: d, encoding: .utf8) else { return "\(o)" }
        return s
    }

    static func dbl(_ v: Any?) -> Double {
        if let n = v as? NSNumber { return n.doubleValue }
        if let s = v as? String { return Double(s) ?? 0 }
        return 0
    }

    static func thermalName(_ s: ProcessInfo.ThermalState) -> String {
        switch s {
        case .nominal: return "nominal"
        case .fair: return "fair"
        case .serious: return "serious"
        case .critical: return "critical"
        @unknown default: return "unknown"
        }
    }
}
