#!/usr/bin/env python3
# Minimal GDB remote-serial-protocol client for Eden's gdbstub (AArch64 guests).
# Usage: gdbrsp.py <port> <script>
# Script lines:  break <hexaddr> | unbreak <hexaddr> | continue | regs | x0 | mem <hexaddr|xN> <len> | bt <hexaddr|sp> <count>
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
        elif op == "breaklr":
            # Break at the current return address (x30 from the last regs/x0).
            lr = regs.get("x30", 0) & 0xFFFFFFFFFFFF
            print("breaklr", hex(lr), "->", rsp.cmd(f"Z0,{lr:x},4"))
        elif op == "unbreakpc":
            pc = regs.get("pc", 0)
            print("unbreakpc", hex(pc), "->", rsp.cmd(f"z0,{pc:x},4"))
        elif op == "unbreak":
            print("unbreak", parts[1], "->", rsp.cmd(f"z0,{int(parts[1], 16):x},4"))
        elif op == "step":
            print("step:", rsp.cmd("s"))
        elif op == "trace":
            # Single-step N instructions, printing pc and x0 (stop early at an optional pc).
            n = int(parts[1])
            stop_at = int(parts[2], 16) if len(parts) > 2 else None
            lo = int(parts[3], 16) if len(parts) > 4 else 0
            hi = int(parts[4], 16) if len(parts) > 4 else 1 << 64
            for _ in range(n):
                rsp.cmd("s")
                r = rsp.regs()
                if lo <= r["pc"] < hi:
                    print(f"  t pc={r['pc']:#x} x0={r['x0']:#x} x8={r['x8']:#x}")
                if stop_at is not None and r["pc"] == stop_at:
                    break
        elif op == "continue":
            print("stop:", rsp.cmd("c"))
        elif op == "x0":
            r = rsp.regs()
            regs = r
            print(f"  pc={r.get('pc', 0):#x} x0={r.get('x0', 0):#x} x1={r.get('x1', 0):#x} lr={r.get('x30', 0):#x}")
        elif op == "regs":
            regs = rsp.regs()
            print(" ".join(f"{k}={v:#x}" for k, v in regs.items()))
        elif op == "mem":
            a = regs[parts[1]] if parts[1] in regs else int(parts[1], 16)
            data = rsp.mem(a, int(parts[2], 0))
            for i in range(0, len(data), 16):
                words = [struct.unpack_from("<Q", data, i + j)[0] for j in range(0, min(16, len(data) - i) - 7, 8)]
                print(f"  {a + i:#x}: " + " ".join(f"{w:016x}" for w in words))
        elif op == "fp":
            # Walk the frame-pointer chain: print return addresses.
            fp = regs.get("x29", 0)
            for _ in range(int(parts[1]) if len(parts) > 1 else 12):
                d = rsp.mem(fp, 16)
                if len(d) < 16:
                    break
                nfp, ret = struct.unpack("<QQ", d)
                print(f"  frame fp={fp:#x} ret={ret & 0xFFFFFFFFFFFF:#x}")
                if nfp <= fp:
                    break
                fp = nfp
        elif op == "quit":
            break
    rsp.s.close()


if __name__ == "__main__":
    main()
