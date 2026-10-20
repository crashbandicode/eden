# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("edenctl", Path(__file__).parents[1] / "edenctl.py")
edenctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(edenctl)


class TracepointCLI(unittest.TestCase):
    def test_exact_addresses_and_register_relative_capture(self):
        with patch.object(edenctl.Client, "request", return_value={"ok": True}) as request:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(edenctl.main([
                    "--socket", "/tmp/tracepoint-test.sock", "tracepoint", "arm",
                    "0x123456789abcdef0", "--register", "x0", "--register", "pc",
                    "--memory-register", "x1", "--memory-offset", "-16",
                    "--memory-size", "32", "--timeout-ms", "1200",
                ]), 0)
            request.assert_called_once_with("tracepoint.arm", {
                "address": "0x123456789abcdef0", "registers": ["x0", "pc"],
                "memory_register": "x1", "memory_offset": -16,
                "memory_size": 32, "timeout_ms": 1200,
            })

    def test_status_and_cancel(self):
        for command in ("status", "cancel"):
            with self.subTest(command=command):
                with patch.object(edenctl.Client, "request", return_value={"ok": True}) as request:
                    with contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(edenctl.main([
                            "--socket", "/tmp/tracepoint-test.sock", "tracepoint", command,
                        ]), 0)
                    request.assert_called_once_with("tracepoint." + command, {})

    def test_ambiguous_buffer_rejected_without_request(self):
        with patch.object(edenctl.Client, "request") as request:
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                edenctl.main([
                    "--socket", "/tmp/tracepoint-test.sock", "tracepoint", "arm",
                    "0x1000", "--register", "x0",
                    "--memory-register", "x1", "--memory-address", "0x2000",
                ])
            request.assert_not_called()


if __name__ == "__main__":
    unittest.main()
