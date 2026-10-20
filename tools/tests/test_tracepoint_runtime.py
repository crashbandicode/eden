#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production control/JIT/PhysicalCore path on tracepoint_guest.nro.

Launch the fixture with rng_seed_enabled=true, rng_seed=0 and the GDB stub off.
This script never sends a game.resume request. Output contains checks, not captures.
"""
import argparse
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from edenctl import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--oracle", type=Path, required=True)
    args = parser.parse_args()
    oracle = json.loads(args.oracle.read_text())
    client = Client(args.socket)

    def call(method, params=None):
        response = client.request(method, params)
        if response.get("ok") is not True:
            raise AssertionError(f"{method}: {response.get('error', {}).get('code')}")
        return response["result"]

    def terminal():
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            result = call("tracepoint.status")
            if result["state"] != "armed":
                return result
            time.sleep(0.01)
        raise AssertionError("tracepoint remained armed beyond deadline")

    config = {"address": oracle["hit"], "registers": ["x0", "x1", "x21", "pc"],
              "memory_register": "x1", "memory_size": 64, "timeout_ms": 5000}
    counts = []
    ids = []
    for _ in range(2):
        call("tracepoint.arm", config)
        result = terminal()
        assert result["state"] == "captured"
        regs = result["registers"]
        assert int(regs["x0"], 16) == int(oracle["register_x0"], 16)
        assert int(regs["pc"], 16) == int(oracle["hit"], 16)
        assert int(regs["x1"], 16) == int(oracle["buffer"], 16)
        memory = bytes.fromhex(result["memory"]["hex"])
        assert not result["memory"]["error"] and len(memory) == 64
        assert memory[8:].hex() == oracle["buffer_tail_hex"]
        assert int.from_bytes(memory[:8], "little") == int(regs["x21"], 16)
        counts.append(int(regs["x21"], 16))
        ids.append(result["id"])
        time.sleep(0.05)
        assert call("tracepoint.status")["id"] == result["id"]
    assert counts[1] > counts[0] and ids[1] != ids[0]

    unreachable = dict(config, address="0x80000400", timeout_ms=500)
    call("tracepoint.arm", unreachable)
    busy = client.request("tracepoint.arm", config)
    assert busy.get("ok") is False and busy["error"]["code"] == "tracepoint_busy"
    assert terminal()["state"] == "expired"
    call("tracepoint.arm", dict(unreachable, timeout_ms=5000))
    assert call("tracepoint.cancel")["state"] == "cancelled"
    bad_memory = {key: value for key, value in config.items() if key != "memory_register"}
    bad_memory["memory_address"] = "0xfffffffffffff000"
    call("tracepoint.arm", bad_memory)
    result = terminal()
    assert result["state"] == "captured" and result["memory"]["hex"] == ""
    assert result["memory"]["error"] == "memory_unavailable"
    prior_count = int(result["registers"]["x21"], 16)
    time.sleep(0.05)
    call("tracepoint.arm", config)
    result = terminal()
    assert result["state"] == "captured" and int(result["registers"]["x21"], 16) > prior_count
    print(json.dumps({"status": "passed", "captures": 4, "register_buffer_oracle": True,
                      "counter_advanced_without_resume": True, "busy_refused": True,
                      "expired": True, "cancelled": True, "unmapped_buffer_continued": True,
                      "oracle_fixture_sha256": oracle["nro_sha256"]}))


if __name__ == "__main__":
    main()
