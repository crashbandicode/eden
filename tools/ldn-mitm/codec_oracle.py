#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare the exact saved C++ literals with an independent wire representation.

Reference facts: spacemeowx2/ldn_mitm 9a2639eff5b2e60065be3dfae69c46de6c3d7648
and Ryujinx contributors c14ce4d2a5c9b373fb454906a6dc142c028d7be2.
Original Python; no reference implementation bodies or game data.
This does not execute or validate the production C++.
"""
import hashlib
import itertools
from pathlib import Path
import re
import struct


def zero_run_representation(payload):
    # Describe each equal-byte group as independent 256-byte zero blocks.
    result = bytearray()
    for value, group in itertools.groupby(payload):
        count = len(list(group))
        if value:
            result.extend(bytes([value]) * count)
        else:
            full, remainder = divmod(count, 256)
            result.extend(b'\x00\xff' * full)
            if remainder:
                result.extend(bytes([0, remainder - 1]))
    return bytes(result)


def main():
    root = Path(__file__).resolve().parents[2]
    fixture = root / 'src/tests/core/ldn_mitm_fixtures.h'
    tests = root / 'src/tests/core/ldn_mitm_codec.cpp'
    saved = fixture.read_text(encoding='utf-8')
    rows = re.findall(r'\{"(\w+)", (\d+), (\d+), "([0-9a-f]+)"\}', saved)
    assert len(rows) == 4, 'four saved golden vectors required'
    for name, kind, size, literal in rows:
        kind, size = int(kind), int(size)
        assert size == (0, 1152, 64, 1152)[kind]
        payload = bytes(size)
        body = zero_run_representation(payload)
        expected = struct.pack('<IBBHH2x', 0x11451400, kind, int(size != 0),
                               len(body), size) + body
        assert bytes.fromhex(literal) == expected, f'{name}: saved bytes differ'
    saved_tests = tests.read_text(encoding='utf-8')
    edges = re.findall(r'\{(1|2|255|256|257), "([0-9a-f]+)"\}', saved_tests)
    assert {int(run) for run, _ in edges} == {1, 2, 255, 256, 257}
    for run, literal in edges:
        run = int(run)
        payload = bytes(run) + b'\x7e' + bytes(1151 - run)
        assert bytes.fromhex(literal) == zero_run_representation(payload), \
            f'zero run {run}: saved edge differs'
    for path in (fixture, tests):
        print(f'{path.relative_to(root)} sha256={hashlib.sha256(path.read_bytes()).hexdigest()}')
    print('PASS exact saved literals: 4 golden frames, 5 compression edges')


if __name__ == '__main__':
    main()
