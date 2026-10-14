# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the named macro interface without a running emulator."""
import contextlib
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("edenctl", Path(__file__).parents[1] / "edenctl.py")
edenctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(edenctl)


class MacroCLI(unittest.TestCase):
    def test_requests(self):
        cases = [
            (["play", "--name", "Training menu"], "macro.play", {"name": "Training menu"}),
            (["play", "--path", "C:/macros/route.json", "--player", "8"], "macro.play",
             {"path": "C:/macros/route.json", "player": 8}),
            (["record", "Route"], "macro.record", {"name": "Route", "player": 0, "countdown_ms": 3000}),
            (["record-from", "--name", "Route", "--marker", "CSS", "--new-name", "Mario"],
             "macro.record_from", {"name": "Route", "marker": "CSS", "new_name": "Mario", "countdown_ms": 3000}),
            (["marker", "--name", "CSS"], "macro.marker", {"name": "CSS"}),
            (["marker"], "macro.marker", {}),
            (["bind", "--name", "Mario", "--keyboard", "Ctrl+Alt+1", "--gamepad", "l+r+plus"],
             "macro.bind", {"name": "Mario", "keyboard": "Ctrl+Alt+1", "gamepad": "l+r+plus"}),
            (["bind", "--name", "Mario"], "macro.bind", {"name": "Mario", "keyboard": "", "gamepad": ""}),
            (["shortcuts", "--marker-keyboard", "Ctrl+M", "--marker-gamepad", "l+r"],
             "macro.shortcuts", {"marker_keyboard": "Ctrl+M", "marker_gamepad": "l+r"}),
            (["inspect", "--name", "Mario"], "macro.inspect", {"name": "Mario"}),
            *[([name], "macro." + name, {}) for name in ("list", "status", "stop", "shortcuts")],
        ]
        for args, method, params in cases:
            with self.subTest(args=args), patch.object(edenctl.Client, "request", return_value={"ok": True}) as request:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(edenctl.main(["--socket", "/tmp/eden-test.sock", "macro", *args]), 0)
                request.assert_called_once_with(method, params)

    def test_source_is_unambiguous(self):
        for args in (["play"], ["play", "--name", "Route", "--path", "/tmp/route.json"],
                     ["play", "--name", "Route", "--player", "10"]):
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()), patch.object(edenctl.Client, "request") as request:
                with self.assertRaises(SystemExit):
                    edenctl.main(["--socket", "/tmp/eden-test.sock", "macro", *args])
                request.assert_not_called()


if __name__ == "__main__":
    unittest.main()
