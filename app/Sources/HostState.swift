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
        // Legacy RWX arena (page-touch experiments; not the plan on TXM boxes).
        let arenaRC = rl_jit_arena_init(Int(jitArenaMB) << 20)
        rl_log_str("host: jit arena init rc=\(arenaRC) \(HostState.fill(512) { rl_jit_arena_json($0, 512) })")
        // TXM JIT pool (jit26.c): wait for the debugger, prepare, remap, self-test.
        if let m = try? String(contentsOf: jit26MarkerFile, encoding: .utf8), !m.isEmpty {
            lock.lock(); lastJitKill = m; lock.unlock()
            rl_log_str("host: previous run died inside a jit26 step: \(m.trimmingCharacters(in: .whitespacesAndNewlines))")
            try? FileManager.default.removeItem(at: jit26MarkerFile)
        }
        rl_jit26_set_marker_path(jit26MarkerFile.path)
        rl_log_str("host: config \(HostState.compact(config))")
        if cfgBool("jit_autostart", true) {
            startJit26()
        } else {
            rl_log_str("host: jit26 autostart disabled (cfg jit_autostart=0); use /run jitprep")
        }
        // FEX's executable memory comes out of the jit26 pool.
        rlfexBind()
        workQueue.async { self.runProbes() }
    }

    var jitArenaMB: Int { infoInt("RLJitArenaMB", 16) }
    var jitPoolMB: Int { cfgInt("jit_pool_mb", infoInt("RLJitPoolMB", 128)) }
    var jitWaitS: Int { cfgInt("jit_wait_s", infoInt("RLJitWaitS", 60)) }
    var jit26MarkerFile: URL { cachesDir.appendingPathComponent("jit26-inflight.txt") }

    func infoInt(_ key: String, _ def: Int) -> Int {
        (Bundle.main.object(forInfoDictionaryKey: key) as? NSNumber)?.intValue
            ?? Int((Bundle.main.object(forInfoDictionaryKey: key) as? String) ?? "") ?? def
    }

    // MARK: - runtime config (Caches/rl-config.json, edited with /run cfg k=v)

    var configFile: URL { cachesDir.appendingPathComponent("rl-config.json") }
    lazy var config: [String: Any] = {
        guard let d = try? Data(contentsOf: configFile),
              let o = try? JSONSerialization.jsonObject(with: d), let dict = o as? [String: Any] else { return [:] }
        return dict
    }()

    func cfgInt(_ k: String, _ def: Int) -> Int {
        if let n = config[k] as? NSNumber { return n.intValue }
        if let s = config[k] as? String, let n = Int(s) { return n }
        return def
    }

    func cfgBool(_ k: String, _ def: Bool) -> Bool {
        if let n = config[k] as? NSNumber { return n.boolValue }
        if let s = config[k] as? String { return ["1", "true", "yes", "on"].contains(s.lowercased()) }
        return def
    }

    func setConfig(_ kv: [String: Any]) -> [String: Any] {
        lock.lock()
        for (k, v) in kv {
            if let s = v as? String, s.isEmpty { config.removeValue(forKey: k) } else { config[k] = v }
        }
        let c = config
        lock.unlock()
        if let d = try? JSONSerialization.data(withJSONObject: c, options: [.sortedKeys, .prettyPrinted]) {
            try? d.write(to: configFile, options: .atomic)
        }
        return c
    }

    /// Flags for rl_jit26_start from the runtime config.
    var jit26Flags: Int32 {
        // Values of the RL_JIT26_* flags in jit26.h (anonymous C enum).
        var f: Int32 = 0
        if cfgBool("jit_in_place", false) { f |= 1 }   // RL_JIT26_IN_PLACE
        if !cfgBool("jit_detach", true) { f |= 2 }     // RL_JIT26_NO_DETACH
        if !cfgBool("jit_selftest", true) { f |= 4 }   // RL_JIT26_NO_SELFTEST
        return f
    }

    @discardableResult
    func startJit26(waitS: Int? = nil) -> Int32 {
        let rc = rl_jit26_start(jitPoolMB << 20, Int32(waitS ?? jitWaitS), jit26Flags)
        rl_log_str("host: jit26 start rc=\(rc) pool=\(jitPoolMB) MB wait=\(waitS ?? jitWaitS) s flags=\(jit26Flags)")
        return rc
    }

    func jit26Info() -> [String: Any] {
        HostState.parseJSON(HostState.fill(2048) { rl_jit26_status_json($0, 2048) })
    }

    // MARK: - FEXCore (rlfex)

    private var fexInit: [String: Any] = ["ok": false, "stage": "not-initialised"]
    func fexInfo() -> [String: Any] {
        var d: [String: Any] = [:]
        lock.lock(); d["init"] = fexInit; lock.unlock()
        #if RL_HAVE_FEX
        d["status"] = HostState.parseJSON(HostState.fill(1024) { rlfex_status_json($0, 1024) })
        d["linked"] = true
        #else
        d["linked"] = false
        d["note"] = "app built without core/fex (see the build log's FEX step)"
        #endif
        return d
    }

    private func rlfexBind() {
        #if RL_HAVE_FEX
        rlfex_set_log { line in if let line = line { rl_log_str(String(cString: line)) } }
        rlfex_set_exec_allocator({ size, rxOut in rl_jit26_alloc(size, rxOut) }, { rw, size in rl_jit26_free(rw, size) })
        rlk_set_log { line in if let line = line { rl_log_str(String(cString: line)) } }
        rl_log_str("host: rlfex bound to the jit26 pool; kernel log bound")
        #else
        rl_log_str("host: rlfex not linked in this build")
        #endif
    }

    /// Initialises FEXCore (needs the jit26 pool ready). Returns the init JSON.
    @discardableResult
    func fexInitNow() -> [String: Any] {
        #if RL_HAVE_FEX
        guard rl_jit26_ready() != 0 else {
            let d: [String: Any] = ["ok": false, "error": "jit26 pool not ready", "jit": jit26Info()]
            lock.lock(); fexInit = d; lock.unlock()
            return d
        }
        let s = HostState.fill(4096) { _ = rlfex_init($0, 4096) }
        let d = HostState.parseJSON(s)
        lock.lock(); fexInit = d; lock.unlock()
        rl_log_str("host: rlfex init \(s.prefix(400))")
        return d
        #else
        return ["ok": false, "error": "rlfex not linked"]
        #endif
    }

    func fexSelftest(_ which: String) -> [String: Any] {
        #if RL_HAVE_FEX
        let i = fexInitNow()
        if (i["ok"] as? Bool) != true { return ["ok": false, "init": i] }
        let s = HostState.fill(16384) { _ = rlfex_selftest(which, $0, 16384) }
        var d = HostState.parseJSON(s)
        d["init"] = i
        rl_log_str("host: fex selftest \(which): \(s.prefix(600))")
        return d
        #else
        return ["ok": false, "error": "rlfex not linked"]
        #endif
    }

    func fexRunHex(_ hex: String, rdi: UInt64, rsi: UInt64, rdx: UInt64) -> [String: Any] {
        #if RL_HAVE_FEX
        let i = fexInitNow()
        if (i["ok"] as? Bool) != true { return ["ok": false, "init": i] }
        var bytes: [UInt8] = []
        var h = hex.replacingOccurrences(of: " ", with: "")
        if h.count % 2 == 1 { h = "0" + h }
        var idx = h.startIndex
        while idx < h.endIndex {
            let next = h.index(idx, offsetBy: 2)
            guard let b = UInt8(h[idx..<next], radix: 16) else { return ["ok": false, "error": "bad hex"] }
            bytes.append(b)
            idx = next
        }
        let s = HostState.fill(2048) { out in
            bytes.withUnsafeBufferPointer { p in _ = rlfex_run_bare(p.baseAddress, p.count, rdi, rsi, rdx, out, 2048) }
        }
        rl_log_str("host: fex run \(bytes.count) bytes: \(s)")
        return HostState.parseJSON(s)
        #else
        return ["ok": false, "error": "rlfex not linked"]
        #endif
    }

    // MARK: - fake kernel (rlkernel)

    func kernelInfo() -> [String: Any] {
        #if RL_HAVE_FEX
        var d = HostState.parseJSON(HostState.fill(65536) { rlk_status_json($0, 65536) })
        d["linked"] = true
        return d
        #else
        return ["linked": false, "note": "app built without core/fex (kernel not linked)"]
        #endif
    }

    /// Starts (or, with dryRun, only loads) a guest program in the fake kernel.
    func guestExec(_ argv: [String], env: [String], cwd: String, dryRun: Bool) -> [String: Any] {
        #if RL_HAVE_FEX
        guard !argv.isEmpty else { return ["ok": false, "error": "empty argv"] }
        let cargv: [UnsafePointer<CChar>?] = argv.map { UnsafePointer(strdup($0)) }
        let cenv: [UnsafePointer<CChar>?] = env.map { UnsafePointer(strdup($0)) }
        defer {
            for p in cargv { if let p = p { free(UnsafeMutablePointer(mutating: p)) } }
            for p in cenv { if let p = p { free(UnsafeMutablePointer(mutating: p)) } }
        }
        var rc: Int32 = 0
        let s = HostState.fill(131072) { out in
            cargv.withUnsafeBufferPointer { a in
                cenv.withUnsafeBufferPointer { e in
                    rc = rlk_exec(a.baseAddress, Int32(a.count), e.baseAddress, Int32(e.count), cwd, dryRun ? 1 : 0, out, 131072)
                }
            }
        }
        var d = HostState.parseJSON(s)
        d["rc"] = Int(rc)
        rl_log_str("host: guest exec\(dryRun ? " (dry-run)" : "") \(argv.joined(separator: " ")) → \(s.prefix(300))")
        return d
        #else
        return ["ok": false, "error": "kernel not linked (app built without core/fex)"]
        #endif
    }

    func guestOutput(_ pid: Int32) -> String {
        #if RL_HAVE_FEX
        return HostState.fill(65536) { _ = rlk_process_output(pid, $0, 65536) }
        #else
        return ""
        #endif
    }

    func guestKillAll() -> Int {
        #if RL_HAVE_FEX
        return Int(rlk_kill_all())
        #else
        return 0
        #endif
    }

    /// A file of the guest filesystem (any layer); failure = -errno (Linux numbering).
    func guestReadFile(_ path: String) -> Result<Data, Int32> {
        #if RL_HAVE_FEX
        var buf: UnsafeMutablePointer<UInt8>? = nil
        var len: Int = 0
        let rc = rlk_read_file(path, &buf, &len)
        if rc != 0 { return .failure(rc) }
        defer { free(buf) }
        guard let b = buf else { return .success(Data()) }
        return .success(Data(bytes: b, count: len))
        #else
        return .failure(-38)
        #endif
    }

    /// Creates or replaces a file in the guest's writable layer. 0 or -errno.
    func guestWriteFile(_ path: String, _ data: Data) -> Int32 {
        #if RL_HAVE_FEX
        if data.isEmpty { return rlk_write_file(path, nil, 0) }
        return data.withUnsafeBytes { raw in
            rlk_write_file(path, raw.bindMemory(to: UInt8.self).baseAddress, data.count)
        }
        #else
        return -38
        #endif
    }

    func guestListDir(_ path: String) -> [String: Any] {
        #if RL_HAVE_FEX
        return HostState.parseJSON(HostState.fill(1 << 20) { _ = rlk_list_dir(path, $0, 1 << 20) })
        #else
        return ["ok": false, "error": "kernel not linked (app built without core/fex)"]
        #endif
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
            "jit": jit26Info(),          // TXM pool (jit26.c): stage, pool, rw_alias, tests
            "jit_probe": jitInfo(),      // legacy map-only RWX probe / arena exec test
            "jit_arena": jitArenaInfo(),
            "fex": fexInfo(),
            "config": { lock.lock(); defer { lock.unlock() }; return config }(),
            "last_jit_kill": { lock.lock(); defer { lock.unlock() }; return lastJitKill }(),
            "core": coreInfo(),
            "vfs": vfsStats(),
            "va": vaInfo(),
            "mem": memInfo(),
            "frames": renderer?.frameCount ?? 0,
            "requests": httpd_request_count(),
            "log_next": rl_log_next_seq(),
            "guest": kernelInfo(),      // fake kernel: processes, syscall count, fex ready
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
            // jittest [--legacy [--trust] [--page N] [--madvise] [--fresh]]
            // Default: re-run the write/execute/rewrite/execute test inside the
            // TXM pool (jit26) and return the pool status. --legacy runs the old
            // RWX-arena flow (page-touch experiments).
            let flags = Array(argv.dropFirst())
            if !flags.contains("--legacy") {
                let s = HostState.fill(2048) { _ = rl_jit26_selftest_json($0, 2048) }
                let d = HostState.parseJSON(s)
                return ["ok": (d["ok"] as? Bool) ?? false, "executed": rl_jit26_ready() != 0, "test": d, "jit": jit26Info()]
            }
            let trust = flags.contains("--trust") || flags.contains("--force")
            var page: Int32 = 0
            if let i = flags.firstIndex(of: "--page"), i + 1 < flags.count { page = Int32(flags[i + 1]) ?? 0 }
            let madv = flags.contains("--madvise")
            let fresh = flags.contains("--fresh")
            if !trust {
                let d = runJitTest()
                return ["ok": false, "executed": false, "jit": d, "arena": jitArenaInfo(),
                        "hint": "legacy flow: authorize the arena with a debugger write per page, then jittest --legacy --trust"]
            }
            let d = runJitExecTest(page: page, madvise: madv, fresh: fresh)
            return ["ok": (d["ok"] as? Bool) ?? false, "executed": true, "jit": d, "arena": jitArenaInfo()]
        case "jitprep":
            // jitprep [--wait S]: (re)start the TXM preparation (waits for P_TRACED).
            let flags = Array(argv.dropFirst())
            var wait: Int? = nil
            if let i = flags.firstIndex(of: "--wait"), i + 1 < flags.count { wait = Int(flags[i + 1]) }
            if rl_jit26_ready() != 0 { return ["ok": true, "already_ready": true, "jit": jit26Info()] }
            let stage = String(cString: rl_jit26_stage())
            if stage == "waiting-for-debugger" || stage == "preparing" || stage == "remapping" || stage == "testing" || stage == "detaching" {
                return ["ok": true, "in_progress": true, "jit": jit26Info()]
            }
            // A failed or never-started preparation: run it synchronously on the work queue.
            let w = wait ?? jitWaitS
            workQueue.async { _ = rl_jit26_prepare_now(self.jitPoolMB << 20, Int32(w), self.jit26Flags) }
            return ["ok": true, "started": true, "wait_s": w, "flags": jit26Flags, "jit": jit26Info()]
        case "jitdetach":
            let ok = rl_jit26_detach()
            return ["ok": ok != 0, "jit": jit26Info()]
        case "cfg":
            // cfg key=value ... (empty value deletes). Keys: jit_pool_mb, jit_wait_s,
            // jit_in_place, jit_detach, jit_selftest, jit_autostart. Applied at next launch
            // (jitprep uses them immediately).
            var kv: [String: Any] = [:]
            for a in argv.dropFirst() {
                let parts = a.split(separator: "=", maxSplits: 1).map(String.init)
                guard parts.count == 2 else { return ["ok": false, "error": "expected key=value", "arg": a] }
                if let n = Int(parts[1]) { kv[parts[0]] = n } else { kv[parts[0]] = parts[1] }
            }
            return ["ok": true, "config": setConfig(kv), "file": configFile.path]
        case "fex-init":
            return fexInitNow()
        case "fex-selftest":
            return fexSelftest(argv.count > 1 ? argv[1] : "all")
        case "fex-run":
            // fex-run HEXBYTES [rdi] [rsi] [rdx]  (code must end with hlt; rax = result)
            guard argv.count > 1 else { return ["ok": false, "error": "usage: fex-run HEX [rdi rsi rdx]"] }
            let rdi = argv.count > 2 ? (UInt64(argv[2]) ?? 0) : 0
            let rsi = argv.count > 3 ? (UInt64(argv[3]) ?? 0) : 0
            let rdx = argv.count > 4 ? (UInt64(argv[4]) ?? 0) : 0
            return fexRunHex(argv[1], rdi: rdi, rsi: rsi, rdx: rdx)
        case "vaprobe":
            let limit = argv.count > 1 ? (Int32(argv[1]) ?? 1024) : 1024
            return ["ok": true, "va": runVaProbe(stepLimitGB: limit)]
        case "vaprobe2":
            // Reservation-limit experiments (see rl_va_probe2_json); takes seconds.
            let s = HostState.fill(4096) { rl_va_probe2_json($0, 4096) }
            rl_log_str("host: va probe 2 \(s)")
            return ["ok": true, "va2": HostState.parseJSON(s), "mem": memInfo()]
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
            let d = HostState.parseJSON(s)
            #if RL_HAVE_FEX
            // The kernel serves guest files from this mount from now on.
            if (d["ok"] as? Bool) == true { rlk_set_vfs(rlcore_vfs_handle()) }
            #endif
            return d
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
        case "exec":
            // exec [--dry-run] /guest/path [args...]  — env and cwd come from the
            // request (empty env → the kernel's default PATH/HOME/...). dry-run
            // loads the ELF + interpreter and reports the layout without running.
            var rest = Array(argv.dropFirst())
            var dry = false
            if rest.first == "--dry-run" { dry = true; rest.removeFirst() }
            guard !rest.isEmpty else { return ["ok": false, "error": "usage: exec [--dry-run] /guest/path [args...]"] }
            return guestExec(rest, env: env, cwd: cwd, dryRun: dry)
        case "ps":
            return ["ok": true, "kernel": kernelInfo()]
        case "guest-out":
            guard argv.count > 1, let pid = Int32(argv[1]) else { return ["ok": false, "error": "usage: guest-out PID"] }
            return ["ok": true, "pid": Int(pid), "output": guestOutput(pid)]
        case "killall":
            return ["ok": true, "stopped": guestKillAll()]
        default:
            if cmd.hasPrefix("/") {
                // A guest program: POST /run {"argv":["/opt/rl/bin/hello-static"],"env":[],"cwd":"/"}
                return guestExec(argv, env: env, cwd: cwd, dryRun: false)
            }
            return [
                "ok": false,
                "error": "unknown builtin (guest programs are given as absolute guest paths)",
                "builtins": ["exec [--dry-run] /guest/path [args...]", "ps", "guest-out PID", "killall",
                             "jittest [--legacy [--trust] [--page N] [--madvise] [--fresh]]", "jitprep [--wait S]", "jitdetach",
                             "cfg key=value ...", "fex-init", "fex-selftest [add|loop|sse|call|mem|syscall|exit|all]",
                             "fex-run HEX [rdi rsi rdx]", "vaprobe [steps]", "vaprobe2", "memprobe",
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
        let j26 = jit26Info()
        let stage26 = (j26["stage"] as? String) ?? "?"
        if (j26["ok"] as? Bool) == true {
            let at = (j26["attached_test"] as? Bool) == true ? "ok" : "\(j26["attached_test"] ?? "-")"
            let dt: String = {
                if let b = j26["detached_test"] as? Bool { return b ? "ok" : "FAIL" }
                return (j26["detached"] as? Bool) == true ? "?" : "not-detached"
            }()
            lines.append("JIT ok   pool \(j26["size_mb"] ?? "?") MB rx \(j26["pool"] ?? "?") rw \(j26["rw_alias"] ?? "?") by \(j26["prepared_by"] ?? "?")  attached-test \(at)  detached-test \(dt)  ptraced=\(j26["ptraced"] ?? "?")")
        } else {
            let err = (j26["error"] as? String) ?? ""
            lines.append("JIT  \(stage26)  waited \(j26["waited_s"] ?? "?")/\(j26["wait_s"] ?? "?") s  ptraced=\(j26["ptraced"] ?? "?") traps=\(j26["unserviced_traps"] ?? "?")  \(err.isEmpty ? "(needs the laptop JIT helper: tv.py jit)" : err)")
        }
        let arena = jitArenaInfo()
        lines.append("ARENA legacy \(arena["prot"] ?? "?") \(arena["size_mb"] ?? "?") MB (rwx_errno \(arena["rwx_errno"] ?? "?"))   probe: \(j["stage"] ?? (j["ok"] as? Bool == true ? "ok" : "?")) rwx_errno=\(j["rwx_errno"] ?? "?")")
        let fx = fexInfo()
        let fi = (fx["init"] as? [String: Any]) ?? [:]
        if (fx["linked"] as? Bool) != true {
            lines.append("FEX  not linked in this build")
        } else if (fi["ok"] as? Bool) == true {
            let st = (fx["status"] as? [String: Any]) ?? [:]
            lines.append("FEX  \(fi["version"] ?? "?") ready  runs=\(st["runs"] ?? 0) syscalls=\(st["syscalls"] ?? 0) poisoned=\(st["poisoned"] ?? false)")
        } else {
            lines.append("FEX  linked, \(fi["stage"] as? String ?? "not initialised")  \(fi["error"] ?? "")  (tv.py fex selftest)")
        }
        let kn = kernelInfo()
        if (kn["linked"] as? Bool) == true {
            let procs = (kn["processes"] as? [[String: Any]]) ?? []
            let running = procs.filter { ($0["state"] as? String) == "running" }.count
            let last = procs.last.map { "last: pid \($0["pid"] ?? 0) \($0["exe"] ?? "") \($0["state"] ?? "") exit=\($0["exit_code"] ?? 0) syscalls=\($0["syscalls"] ?? 0)" } ?? "no guest process yet (tv.py exec)"
            lines.append("KERN procs \(procs.count) (\(running) running)  syscalls \(kn["syscalls_total"] ?? 0)  fex \(kn["fex"] ?? false)  \(last)")
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
