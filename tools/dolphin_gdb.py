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
    elif sys.argv[2] == "peek":
        a, n = int(sys.argv[3], 16), int(sys.argv[4], 16)
        print(g.mem(a, n).hex())


if __name__ == "__main__":
    main()
