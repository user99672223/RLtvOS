#!/usr/bin/env python3
"""gdbremote.py — minimal gdb-remote (debugserver) client used by tv.py jit.

Why: on iOS/tvOS 26+ (Trusted Execution Monitor) a page an app writes code
into only becomes executable after *a debugger has written to that page*.
tv.py therefore attaches to the app through Apple's debugserver (reached via
pymobiledevice3 / idevice), writes every 16 KB page of the app's JIT arena
back to itself, and detaches. The app then executes code from that arena.

    gdbremote.py HOST:PORT probe
    gdbremote.py HOST:PORT authorize PID BASE_HEX SIZE [--chunk N] [--no-detach]
    gdbremote.py HOST:PORT read PID ADDR_HEX LEN

Protocol notes (debugserver): $payload#cs framing, QStartNoAckMode, vAttach;PID
(hex) stops the process and returns a T stop reply, m/M hex memory access,
D detaches and resumes. PacketSize from qSupported bounds the chunk size.
"""
import socket
import sys
import time


class GdbRemoteError(Exception):
    pass


class GdbRemote:
    def __init__(self, host, port, timeout=30.0, log=None):
        self.host, self.port, self.timeout = host, int(port), timeout
        self.sock = None
        self.noack = False
        self.packet_size = 0x4000
        self.log = log or (lambda *a: None)

    # ----------------------------------------------------------- framing
    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        r = self.cmd("QStartNoAckMode")
        if r == "OK":
            self.noack = True
        feats = self.cmd("qSupported:xmlRegisters=i386,arm,mips,arc;multiprocess+;swbreak+")
        for f in feats.split(";"):
            if f.startswith("PacketSize="):
                try:
                    self.packet_size = int(f.split("=", 1)[1], 16)
                except ValueError:
                    pass
        self.log(f"connected to {self.host}:{self.port} noack={self.noack} packet_size=0x{self.packet_size:x}")
        return feats

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            finally:
                self.sock = None

    @staticmethod
    def _checksum(payload):
        return sum(payload.encode("latin-1")) & 0xFF

    def _send(self, payload):
        frame = f"${payload}#{self._checksum(payload):02x}".encode("latin-1")
        self.sock.sendall(frame)
        if not self.noack:
            self._expect_ack()

    def _expect_ack(self):
        while True:
            c = self.sock.recv(1)
            if not c:
                raise GdbRemoteError("connection closed waiting for ack")
            if c == b"+":
                return
            if c == b"-":
                raise GdbRemoteError("remote NAKed packet")

    def _recv(self):
        buf = bytearray()
        # skip to '$'
        while True:
            c = self.sock.recv(1)
            if not c:
                raise GdbRemoteError("connection closed")
            if c == b"$":
                break
        while True:
            c = self.sock.recv(1)
            if not c:
                raise GdbRemoteError("connection closed mid-packet")
            if c == b"#":
                break
            buf += c
        cs = self.sock.recv(2)
        while len(cs) < 2:
            more = self.sock.recv(2 - len(cs))
            if not more:
                raise GdbRemoteError("connection closed in checksum")
            cs += more
        payload = buf.decode("latin-1")
        if int(cs, 16) != self._checksum(payload):
            if not self.noack:
                self.sock.sendall(b"-")
            raise GdbRemoteError("bad checksum")
        if not self.noack:
            self.sock.sendall(b"+")
        # unescape 0x7d
        if "}" in payload:
            out, i = [], 0
            while i < len(payload):
                ch = payload[i]
                if ch == "}" and i + 1 < len(payload):
                    out.append(chr(ord(payload[i + 1]) ^ 0x20))
                    i += 2
                else:
                    out.append(ch)
                    i += 1
            payload = "".join(out)
        return payload

    def cmd(self, payload):
        self._send(payload)
        return self._recv()

    # ----------------------------------------------------------- operations
    def attach(self, pid):
        r = self.cmd(f"vAttach;{pid:x}")
        if r.startswith("E"):
            raise GdbRemoteError(f"vAttach failed: {r}")
        self.log(f"attached to pid {pid}: {r[:60]}")
        return r

    def read_mem(self, addr, n):
        r = self.cmd(f"m{addr:x},{n:x}")
        if r.startswith("E") or not r:
            raise GdbRemoteError(f"read {addr:#x}+{n:#x} failed: {r!r}")
        return bytes.fromhex(r)

    def write_mem(self, addr, data):
        r = self.cmd(f"M{addr:x},{len(data):x}:{data.hex()}")
        if r != "OK":
            raise GdbRemoteError(f"write {addr:#x}+{len(data):#x} failed: {r!r}")

    def authorize(self, base, size, chunk=None, progress=None):
        """Write every byte of [base, base+size) back to itself (touching every page)."""
        max_chunk = max(256, (self.packet_size - 64) // 2)   # hex doubles the size
        chunk = min(chunk or 32768, max_chunk)
        done = 0
        t0 = time.time()
        while done < size:
            n = min(chunk, size - done)
            data = self.read_mem(base + done, n)
            if len(data) != n:
                raise GdbRemoteError(f"short read at {base + done:#x}: {len(data)} of {n}")
            self.write_mem(base + done, data)
            done += n
            if progress and (done % (8 << 20) == 0 or done == size):
                progress(done, size, time.time() - t0)
        return done

    def detach(self):
        r = self.cmd("D")
        self.log(f"detach: {r}")
        return r

    def kill(self):
        return self.cmd("k")


def parse_hostport(s):
    s = s.strip()
    if s.startswith("connect://"):
        s = s[len("connect://"):]
    if s.startswith("["):
        host, _, port = s[1:].partition("]")
        return host, int(port.lstrip(":"))
    host, _, port = s.rpartition(":")
    return host, int(port)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    host, port = parse_hostport(argv[0])
    op = argv[1]
    g = GdbRemote(host, port, log=lambda *a: print(*a, file=sys.stderr))
    g.connect()
    try:
        if op == "probe":
            print(g.cmd("qHostInfo"))
            print(g.cmd("qProcessInfo"))
            return 0
        if op == "read":
            pid, addr, n = int(argv[2]), int(argv[3], 16), int(argv[4])
            g.attach(pid)
            print(g.read_mem(addr, n).hex())
            g.detach()
            return 0
        if op == "authorize":
            pid, base, size = int(argv[2]), int(argv[3], 16), int(argv[4], 0)
            chunk = None
            detach = True
            if "--chunk" in argv:
                chunk = int(argv[argv.index("--chunk") + 1], 0)
            if "--no-detach" in argv:
                detach = False
            g.attach(pid)
            n = g.authorize(base, size, chunk,
                            progress=lambda d, s, t: print(f"  {d >> 20} / {s >> 20} MB in {t:.1f}s", file=sys.stderr))
            print(f"authorized {n} bytes ({n // 16384} pages of 16 KB) at {base:#x}")
            if detach:
                g.detach()
            return 0
        print(f"unknown op {op}")
        return 2
    finally:
        g.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
