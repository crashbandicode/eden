#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the original bounded-tracepoint NRO fixture with LLVM's AArch64 assembler."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path, help="new output directory")
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
source = Path(__file__).with_name("tracepoint_guest.s")
obj = args.output / "tracepoint_guest.o"
raw = args.output / "tracepoint_guest.bin"
subprocess.run(["llvm-mc", "-triple=aarch64", "-filetype=obj", str(source), "-o", str(obj)], check=True)
subprocess.run(["llvm-objcopy", "-O", "binary", "--only-section=.text", str(obj), str(raw)], check=True)
symbols = subprocess.check_output(["llvm-nm", "-n", str(obj)], text=True)
offsets = {parts[2]: int(parts[0], 16) for line in symbols.splitlines()
           if len(parts := line.split()) == 3 and parts[2] in {"start", "hit", "buffer"}}
data = bytearray(raw.read_bytes())
assert len(data) == 0x3000 and offsets == {"start": 0x80, "hit": 0x9c, "buffer": 0x2000}
assert struct.unpack_from("<I", data, 0)[0] == 0x14000020  # B +0x80, no unresolved relocation
adr = struct.unpack_from("<I", data, 0x80)[0]
assert adr & 0x9f00001f == 0x10000014  # ADR X20
assert 0x80 + (((adr >> 5) & 0x7ffff) << 2 | ((adr >> 29) & 3)) == offsets["buffer"]
assert struct.unpack_from("<I", data, offsets["hit"])[0] == 0xd503201f  # A64 NOP oracle
struct.pack_into("<I", data, 4, 0x50)
data[8:16] = b"HOMEBREW"
data[0x10:0x14] = b"NRO0"
struct.pack_into("<I", data, 0x18, len(data))
struct.pack_into("<6I", data, 0x20, 0, 0x1000, 0x1000, 0x1000, 0x2000, 0x1000)
nro = args.output / "tracepoint_guest.nro"
nro.write_bytes(data)
assert nro.read_bytes()[0x80:] == raw.read_bytes()[0x80:]
disassembly = subprocess.check_output(["llvm-objdump", "-d", str(obj)], text=True)
(args.output / "assembly.txt").write_text(disassembly)
oracle = {"nro_sha256": hashlib.sha256(data).hexdigest(),
          "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
          "base_with_rng_seed_zero": "0x80000000", "hit": "0x8000009c",
          "buffer": "0x80002000", "register_x0": "0x1234",
          "buffer_tail_hex": (b"\xa5" * 56).hex(),
          "raw_code_matches_nro": True}
(args.output / "oracle.json").write_text(json.dumps(oracle, indent=2) + "\n")
print(json.dumps({"fixture": str(nro), "sha256": oracle["nro_sha256"], "raw_code_matches_nro": True}))
