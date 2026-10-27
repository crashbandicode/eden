#!/usr/bin/env python3
# Minimal GDB remote-serial-protocol client for Eden's gdbstub (AArch64 guests).
# Usage: gdbrsp.py <port> <script>
# Script lines:  break <hexaddr> | unbreak <hexaddr> | continue | regs | mem <hexaddr|xN> <len> | bt <hexaddr|sp> <count>
#                | quit
import socket
import struct
import sys

REGS = [f"x{i}" for i in range(31)] + ["sp", "pc", "cpsr"]


class Rsp:
    def __init__(self, port):
        import time
        for _ in range(240):  # wait up to ~2 min for the stub to listen
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=600)
                break
            except OSError:
                time.sleep(0.5)
        else:
            raise SystemExit("gdbstub not reachable")
        self.buf = b""

    def _read_packet(self):
        while True:
            while b"$" not in self.buf or b"#" not in self.buf[self.buf.index(b"$"):]:
                chunk = self.s.recv(65536)
                if not chunk:
                    raise EOFError
                self.buf += chunk
            start = self.buf.index(b"$")
            end = self.buf.index(b"#", start)
            if len(self.buf) < end + 3:
                self.buf += self.s.recv(65536)
                continue
            data = self.buf[start + 1:end]
            self.buf = self.buf[end + 3:]
            self.s.sendall(b"+")
            return data.decode(errors="replace")

    def cmd(self, c):
        pkt = c.encode()
        self.s.sendall(b"$" + pkt + b"#" + f"{sum(pkt) & 0xff:02x}".encode())
        return self._read_packet()

    def regs(self):
        r = self.cmd("g")
        vals = {}
        off = 0
        for name in REGS:
            n = 8 if name != "cpsr" else 4
            if off + n * 2 > len(r):
                break
            vals[name] = int.from_bytes(bytes.fromhex(r[off:off + n * 2]), "little")
            off += n * 2
        return vals

    def mem(self, addr, length):
        r = self.cmd(f"m{addr:x},{length:x}")
        try:
            return bytes.fromhex(r)
        except ValueError:
            return b""


def main():
    rsp = Rsp(int(sys.argv[1]))
    print("halt reason:", rsp.cmd("?"))
    regs = {}
    for line in open(sys.argv[2]):
        parts = line.split()
        if not parts or parts[0].startswith("#"):
            continue
        op = parts[0]
        if op == "break":
            print("break", parts[1], "->", rsp.cmd(f"Z0,{int(parts[1], 16):x},4"))
        elif op == "unbreak":
            print("unbreak", parts[1], "->", rsp.cmd(f"z0,{int(parts[1], 16):x},4"))
        elif op == "continue":
            print("stop:", rsp.cmd("c"))
        elif op == "regs":
            regs = rsp.regs()
            print(" ".join(f"{k}={v:#x}" for k, v in regs.items()))
        elif op == "mem":
            a = regs[parts[1]] if parts[1] in regs else int(parts[1], 16)
            data = rsp.mem(a, int(parts[2], 0))
            for i in range(0, len(data), 16):
                words = [struct.unpack_from("<Q", data, i + j)[0] for j in range(0, min(16, len(data) - i) - 7, 8)]
                print(f"  {a + i:#x}: " + " ".join(f"{w:016x}" for w in words))
        elif op == "quit":
            break
    rsp.s.close()


if __name__ == "__main__":
    main()
