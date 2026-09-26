import Foundation
import UIKit

/// Routes for the LAN debug endpoint (port 7777). The socket/HTTP work is
/// done by the C server in Native/httpd.c; this file only builds replies.
final class DebugServer {
    static let shared = DebugServer()
    private(set) var port: UInt16 = 0

    struct Reply {
        var status: Int32
        var type: String
        var data: Data
    }

    func start(port: UInt16) {
        self.port = port
        let rc = httpd_start(port) { method, path, query, body, bodyLen, resp in
            guard let resp = resp else { return }
            let m = String(cString: method)
            let p = String(cString: path)
            let q = String(cString: query)
            let b = bodyLen > 0 ? Data(bytes: body, count: bodyLen) : Data()
            let reply = DebugServer.shared.handle(method: m, path: p, query: q, body: b)
            reply.data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> Void in
                httpd_set_response(resp, reply.status, reply.type, raw.baseAddress, reply.data.count)
            }
        }
        if rc == 0 {
            rl_log_str("host: debug server listening on 0.0.0.0:\(port)")
        } else {
            rl_log_str("host: debug server FAILED rc=\(rc)")
        }
    }

    // MARK: - routing

    func handle(method: String, path: String, query: String, body: Data) -> Reply {
        let params = DebugServer.parseQuery(query)
        let host = HostState.shared
        switch (method, path) {
        case ("GET", "/"), ("GET", "/status"):
            return json(host.status())

        case ("GET", "/screenshot"):
            if let png = host.renderer?.snapshotPNG() {
                return Reply(status: 200, type: "image/png", data: png)
            }
            return json(["ok": false, "error": "no renderer"], status: 503)

        case ("GET", "/log"):
            let since = UInt64(params["since"] ?? "") ?? 1
            let maxLines = Int32(params["max"] ?? "") ?? 4000
            if params["format"] == "text" {
                var next: UInt64 = 0
                guard let c = rl_log_text_since(since, maxLines, &next) else { return text("", status: 500) }
                let s = String(cString: c)
                free(c)
                return Reply(status: 200, type: "text/plain; charset=utf-8", data: Data(s.utf8))
            }
            guard let c = rl_log_json_since(since, maxLines) else { return text("", status: 500) }
            let s = String(cString: c)
            free(c)
            return Reply(status: 200, type: "application/json", data: Data(s.utf8))

        case ("GET", "/mem"):
            return json(host.memInfo())

        case ("GET", "/va"):
            if params["probe"] == "1" {
                let limit = Int32(params["steps"] ?? "") ?? 1024
                return json(host.runVaProbe(stepLimitGB: limit))
            }
            return json(host.vaInfo())

        case ("GET", "/jit"):
            if params["run"] == "1" { return json(host.runJitTest()) }
            return json(host.jitInfo())

        case ("GET", "/crash"):
            if let d = FileManager.default.contents(atPath: host.crashFile.path), !d.isEmpty {
                return Reply(status: 200, type: "text/plain; charset=utf-8", data: d)
            }
            return text("no crash report\n", status: 404)

        case ("DELETE", "/crash"):
            try? FileManager.default.removeItem(at: host.crashFile)
            return json(["ok": true])

        case ("POST", "/input"):
            guard let obj = DebugServer.parseBody(body) else { return json(["ok": false, "error": "body must be a JSON object or array"], status: 400) }
            var n = 0
            if let arr = obj as? [[String: Any]] {
                for ev in arr { n = host.enqueueInput(ev) }
            } else if let ev = obj as? [String: Any] {
                n = host.enqueueInput(ev)
            }
            rl_log_str("input: queued (\(n) pending) \(HostState.compact(obj))")
            return json(["ok": true, "queued": n, "note": "no consumer yet (phase A)"])

        case ("POST", "/run"):
            guard let obj = DebugServer.parseBody(body) as? [String: Any],
                  let argv = obj["argv"] as? [String], !argv.isEmpty else {
                return json(["ok": false, "error": "expected {\"argv\":[...],\"env\":[...],\"cwd\":\"...\"}"], status: 400)
            }
            let env = (obj["env"] as? [String]) ?? []
            let cwd = (obj["cwd"] as? String) ?? "/"
            rl_log_str("run: \(argv.joined(separator: " ")) cwd=\(cwd)")
            return json(host.run(argv: argv, env: env, cwd: cwd))

        case ("POST", "/kill"):
            rl_log_str("host: /kill received, exiting")
            DispatchQueue.global().asyncAfter(deadline: .now() + 0.25) { exit(0) }
            return json(["ok": true, "note": "exiting in 250 ms"])

        case ("GET", "/ping"):
            return text("pong\n")

        default:
            if method != "GET" && method != "POST" && method != "DELETE" {
                return json(["ok": false, "error": "method not allowed"], status: 405)
            }
            return json([
                "ok": false, "error": "not found", "path": path,
                "routes": ["GET /status", "GET /screenshot", "GET /log?since=N[&max=M][&format=text]",
                           "GET /mem", "GET /va[?probe=1&steps=N]", "GET /jit[?run=1]", "GET /crash",
                           "DELETE /crash", "POST /input", "POST /run", "POST /kill"],
            ], status: 404)
        }
    }

    // MARK: - helpers

    private func json(_ obj: Any, status: Int32 = 200) -> Reply {
        var o = obj
        if !JSONSerialization.isValidJSONObject(o) { o = ["ok": false, "error": "unserialisable", "repr": "\(obj)"] }
        let d = (try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys, .prettyPrinted])) ?? Data("{}".utf8)
        return Reply(status: status, type: "application/json", data: d + Data("\n".utf8))
    }

    private func text(_ s: String, status: Int32 = 200) -> Reply {
        Reply(status: status, type: "text/plain; charset=utf-8", data: Data(s.utf8))
    }

    static func parseBody(_ body: Data) -> Any? {
        guard !body.isEmpty else { return nil }
        return try? JSONSerialization.jsonObject(with: body, options: [])
    }

    static func parseQuery(_ q: String) -> [String: String] {
        var out: [String: String] = [:]
        for part in q.split(separator: "&") {
            let kv = part.split(separator: "=", maxSplits: 1).map(String.init)
            guard let k = kv.first, !k.isEmpty else { continue }
            let v = kv.count > 1 ? kv[1] : ""
            out[k.removingPercentEncoding ?? k] = v.replacingOccurrences(of: "+", with: " ").removingPercentEncoding ?? v
        }
        return out
    }
}
