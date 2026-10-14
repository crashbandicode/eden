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


class RecordingCLI(unittest.TestCase):
    def test_protocol_requests(self):
        cases = [
            (["start", "C:/clips/game.gif", "--fps", "15", "--width", "800", "--duration-ms", "5000"],
             "recording.start", {"path": "C:/clips/game.gif", "fps": 15, "width": 800, "duration_ms": 5000}),
            (["stop"], "recording.stop", {}),
            (["status"], "recording.status", {}),
        ]
        for args, method, params in cases:
            with self.subTest(method=method), patch.object(edenctl.Client, "request", return_value={"ok": True}) as request:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(edenctl.main(["--socket", "/tmp/eden-test.sock", "recording"] + args), 0)
                request.assert_called_once_with(method, params)


if __name__ == "__main__":
    unittest.main()
