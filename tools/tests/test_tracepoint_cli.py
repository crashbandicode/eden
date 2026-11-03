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
    def test_persistent_trace_keeps_large_ids_and_capture_expression(self):
        import json
        params = {"target": "libtraining_rooms.nro!hook", "max_hits": "9007199254740993",
                  "captures": [{"expression": "[[x0+0x10]+8]", "type": "u32"}]}
        with patch.object(edenctl.Client, "request", return_value={"ok": True}) as request:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(edenctl.main(["--socket", "/tmp/tracepoint-test.sock",
                                               "trace", "arm", json.dumps(params)]), 0)
            request.assert_called_once_with("trace.arm", params)

    def test_trace_cursor_does_not_round_and_windows_method_matches(self):
        with patch.object(edenctl.Client, "request", return_value={"ok": True}) as request:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(edenctl.main(["--socket", "/tmp/tracepoint-test.sock", "trace", "read",
                                               '{"after":"18446744073709551614","limit":4}']), 0)
            request.assert_called_once_with("trace.read", {"after": "18446744073709551614", "limit": 4})

    def test_persistent_trace_rejects_nonobject_before_transport(self):
        with patch.object(edenctl.Client, "request") as request:
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertNotEqual(edenctl.main(["--socket", "/tmp/tracepoint-test.sock", "trace", "arm", "[]"]), 0)
            request.assert_not_called()

    def test_module_and_symbols_commands(self):
        for arguments, method, params in (
                (["modules", "list"], "modules.list", {}),
                (["symbols", "load", "plugin.nro", "/tmp/plugin.elf"], "symbols.load",
                 {"module": "plugin.nro", "elf": "/tmp/plugin.elf"})):
            with self.subTest(method=method), patch.object(edenctl.Client, "request", return_value={"ok": True}) as request:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(edenctl.main(["--socket", "/tmp/tracepoint-test.sock", *arguments]), 0)
                request.assert_called_once_with(method, params)

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

    def test_timeout_values_are_forwarded_exactly(self):
        cases = (
            (("--timeout-ms", "300000"), 300000),
            (("--timeout-ms", "600000"), 600000),
            ((), 5000),
        )
        for timeout_args, expected_timeout in cases:
            with self.subTest(timeout_ms=expected_timeout):
                with patch.object(edenctl.Client, "request", return_value={"ok": True}) as request:
                    with contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(edenctl.main([
                            "--socket", "/tmp/tracepoint-test.sock", "tracepoint", "arm",
                            "0x1000", "--register", "x0", *timeout_args,
                        ]), 0)
                    request.assert_called_once_with("tracepoint.arm", {
                        "address": "0x1000", "registers": ["x0"],
                        "timeout_ms": expected_timeout,
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
