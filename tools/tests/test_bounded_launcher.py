# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Linux integration tests; set EDEN_CONTROL_FIXTURE to the built Qt fixture."""

import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from edenctl import Client, ControlError


def running(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1][0] != "Z"
    except FileNotFoundError:
        return False


@unittest.skipUnless(sys.platform == "linux" and os.environ.get("EDEN_CONTROL_FIXTURE"),
                     "requires Linux, a systemd user manager and EDEN_CONTROL_FIXTURE")
class BoundedLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="eden-bounds-")
        self.directory = Path(self.temporary.name)
        self.output = self.directory / "run"
        self.process = None
        self.stream = (self.directory / "supervisor.log").open("w")

    def tearDown(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=15)
        manifest = self.output / "run.json"
        if manifest.exists():
            unit = json.loads(manifest.read_text())["unit"]
            for verb in ("stop", "reset-failed"):
                subprocess.run(["systemctl", "--user", verb, unit], capture_output=True, timeout=10)
        self.stream.close()
        self.temporary.cleanup()

    def launch(self, descendants=False, debugger=False, runtime=6, hang_timeout=3,
               ignore_term=False):
        command = [os.environ["EDEN_CONTROL_FIXTURE"]]
        self.child_file = self.directory / "child.pid"
        if descendants:
            wrapper = self.directory / "descendant.py"
            wrapper.write_text(
                "import os,sys,subprocess\nfrom pathlib import Path\n"
                "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(300)'])\n"
                "Path(sys.argv[1]).write_text(str(child.pid))\n"
                "os.execv(sys.argv[2],sys.argv[2:])\n")
            command = [sys.executable, str(wrapper), str(self.child_file), *command]
        if ignore_term:
            wrapper = self.directory / "ignore_term.py"
            wrapper.write_text("import os,signal,sys\n"
                               "signal.signal(signal.SIGTERM,signal.SIG_IGN)\n"
                               "os.execv(sys.argv[1],sys.argv[1:])\n")
            command = [sys.executable, str(wrapper), *command]
        self.process = subprocess.Popen(
            [sys.executable, str(TOOLS / "eden-run.py"), "--output", str(self.output),
             "--runtime", str(runtime), "--startup-timeout", "5",
             "--hang-timeout", str(hang_timeout),
             "--memory-gib", "1", *(["--debugger"] if debugger else []), "--", *command],
            env=dict(os.environ, QT_QPA_PLATFORM="offscreen"), stdout=self.stream,
            stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and self.process.poll() is None:
            try:
                return Client(self.output / "control.sock", .3).request("ping")["result"]["pid"]
            except (OSError, ValueError, KeyError, ControlError):
                time.sleep(.05)
        self.fail("fixture did not become ready: " + (self.directory / "supervisor.log").read_text())

    def test_runtime_limit_reaps_descendants(self):
        pid = self.launch(descendants=True)
        child = int(self.child_file.read_text())
        self.process.wait(timeout=15)
        self.assertFalse(running(pid))
        self.assertFalse(running(child))
        manifest = json.loads((self.output / "run.json").read_text())
        self.assertEqual(manifest["reason"], "runtime_limit")
        self.assertEqual(manifest["verified_limits"]["MemoryMax"], "1073741824")
        self.assertEqual(manifest["verified_limits"]["MemorySwapMax"], "0")

    def test_runtime_limit_survives_supervisor_death(self):
        pid = self.launch(descendants=True)
        child = int(self.child_file.read_text())
        self.process.kill()
        self.process.wait(timeout=5)
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline and (running(pid) or running(child)):
            time.sleep(.1)
        self.assertFalse(running(pid), "systemd did not reap the fixture")
        self.assertFalse(running(child), "systemd did not reap its descendant")

    def test_clean_exit_keeps_resource_snapshot(self):
        pid = self.launch(runtime=20)
        deadline = time.monotonic() + 8
        manifest_path = self.output / "run.json"
        while time.monotonic() < deadline:
            manifest = json.loads(manifest_path.read_text())
            if manifest.get("resources_sampled_at"):
                break
            time.sleep(.1)
        self.assertIn("resources_sampled_at", manifest)
        client = Client(self.output / "control.sock")
        client.request("ui.close", {"target": "secondary"})
        client.request("ui.close", {"target": "main"})
        self.process.wait(timeout=10)
        self.assertFalse(running(pid))
        manifest = json.loads(manifest_path.read_text())
        self.assertEqual(manifest["reason"], "exited")
        self.assertEqual(manifest["exit_code"], 0)
        self.assertGreater(manifest["memory_peak_bytes_observed"], 0)
        self.assertEqual(manifest["last_resources"]["MemoryMax"], "1073741824")

    def test_external_stop_timeout_is_not_runtime_limit(self):
        pid = self.launch(runtime=30, hang_timeout=20, ignore_term=True)
        manifest = json.loads((self.output / "run.json").read_text())
        os.kill(pid, signal.SIGSTOP)
        subprocess.run(["systemctl", "--user", "stop", manifest["unit"]],
                       capture_output=True, timeout=10)
        self.process.wait(timeout=10)
        self.assertFalse(running(pid))
        manifest = json.loads((self.output / "run.json").read_text())
        self.assertEqual(manifest["reason"], "stop_timeout")
        self.assertLess(manifest["elapsed_seconds"], 30)

    @unittest.skipUnless(shutil.which("gdb"), "requires gdb")
    def test_debugger_records_crash_stack(self):
        pid = self.launch(debugger=True)
        os.kill(pid, signal.SIGSEGV)
        self.process.wait(timeout=15)
        self.assertFalse(running(pid))
        console = (self.output / "console.log").read_text()
        self.assertIn("SIGSEGV", console)
        self.assertIn("#0 ", console)


if __name__ == "__main__":
    unittest.main()
