#!/usr/bin/env python3
"""Minimal GDB remote-protocol client for Dolphin's GDB stub (Dolphin.ini: [General] GDBPort).

usage: dolphin_gdb.py <port> break <addr> [hits]   run until <addr> is hit; print regs + guest stack
       dolphin_gdb.py <port> peek <addr> <len>       read memory (target must be stopped)
Used to compare the real game's control flow with the recompiled build.
"""
import socket, sys, time


class Gdb:
    def __init__(self, port):
        for _ in range(120):
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=600)
                break
            except OSError:
                time.sleep(1)
        else:
            sys.exit("cannot connect")
        self.buf = b""

    def send(self, cmd):
        pkt = b"$" + cmd.encode() + b"#" + ("%02x" % (sum(cmd.encode()) & 0xFF)).encode()
        self.s.sendall(pkt)
        return self.reply()

    def reply(self):
        while True:
            while b"#" not in self.buf or len(self.buf) < self.buf.index(b"#") + 3:
                d = self.s.recv(65536)
                if not d:
                    raise EOFError
                self.buf += d
            self.buf = self.buf.lstrip(b"+")
            if not self.buf.startswith(b"$"):
                i = self.buf.find(b"$")
                self.buf = self.buf[i:] if i >= 0 else b""
                continue
            end = self.buf.index(b"#")
            data = self.buf[1:end].decode(errors="replace")
            self.buf = self.buf[end + 3:]
            self.s.sendall(b"+")
            return data

    def mem(self, a, n):
        return bytes.fromhex(self.send("m%x,%x" % (a, n)))

    def u32(self, a):
        return int.from_bytes(self.mem(a, 4), "big")

    def regs(self):
        r = self.send("g")
        return [int(r[i:i + 8], 16) for i in range(0, 32 * 8, 8)], r


def main():
    port = int(sys.argv[1])
    g = Gdb(port)
    g.send("?")
    if sys.argv[2] == "break":
        addr = int(sys.argv[3], 16)
        hits = int(sys.argv[4]) if len(sys.argv) > 4 else 1
        print(g.send("Z0,%x,4" % addr))
        for h in range(hits):
            stop = g.send("c")
            gpr, raw = g.regs()
            pc = int(raw[32 * 8 + 64 * 8:32 * 8 + 64 * 8 + 8], 16) if len(raw) > 32 * 8 + 64 * 8 + 8 else 0
            print(f"hit {h}: stop {stop} pc {pc:08X} r3 {gpr[3]:08X} r4 {gpr[4]:08X} r5 {gpr[5]:08X} r1 {gpr[1]:08X}")
            sp = gpr[1]
            for _ in range(12):
                back = g.u32(sp)
                if not (0x80000000 <= back < 0x94000000):
                    break
                print(f"    lr {g.u32(back + 4):08X}")
                sp = back
    elif sys.argv[2] == "sample":   # sample <seconds> <outfile>: periodic halts, record pc/lr chains
        dur, out = float(sys.argv[3]), open(sys.argv[4], "w")

        def drain():
            g.s.setblocking(False)
            try:
                while g.s.recv(65536):
                    pass
            except BlockingIOError:
                pass
            g.s.setblocking(True)
            g.buf = b""

        g.s.sendall(b"$c#63")
        if len(sys.argv) > 5:
            time.sleep(float(sys.argv[5]))   # let the game run untouched first
        t0 = time.time()
        while time.time() - t0 < dur:
            time.sleep(0.03)
            g.s.sendall(b"\x03")
            stop = ""
            while not stop.startswith("T"):
                stop = g.reply()
            f = dict(kv.split(":") for kv in stop[3:].split(";") if ":" in kv)
            pc, sp = int(f.get("40", "0"), 16), int(f.get("01", "0"), 16)
            time.sleep(0.01)
            drain()
            lr = g.send("p43")
            chain = [pc] + ([int(lr, 16)] if len(lr) == 8 else [])
            for _ in range(16):
                back = g.u32(sp)
                if not (0x80000000 <= back < 0x94000000):
                    break
                chain.append(g.u32(back + 4))
                sp = back
            out.write("%.2f %s\n" % (time.time() - t0, " ".join("%08X" % x for x in chain)))
            out.flush()
            drain()
            g.s.sendall(b"$c#63")
    elif sys.argv[2] == "peek":   # peek <after seconds> <addr:len>[,...]  ('*addr' = dereference first)
        g.s.sendall(b"$c#63")
        time.sleep(float(sys.argv[3]))
        g.s.sendall(b"\x03")
        stop = ""
        while not stop.startswith("T"):
            stop = g.reply()
        time.sleep(0.05)
        g.s.setblocking(False)
        try:
            while g.s.recv(65536):
                pass
        except BlockingIOError:
            pass
        g.s.setblocking(True)
        g.buf = b""
        for spec in sys.argv[4].split(","):
            a, n = spec.split(":")
            if a.startswith("*"):
                a = "%x" % g.u32(int(a[1:], 16))
            a, n = int(a, 16), int(n, 16)
            data = g.mem(a, n)
            for i in range(0, n, 16):
                print("%08X: %s" % (a + i, " ".join(data[i + k:i + k + 4].hex().upper() for k in range(0, min(16, n - i), 4))))


if __name__ == "__main__":
    main()
