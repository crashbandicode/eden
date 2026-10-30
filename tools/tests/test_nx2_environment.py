# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""NX2 launch construction tests: no emulator or systemd service is started."""

import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from nx2_environment import NX2_ENVIRONMENT_VARIABLES, systemd_nx2_environment

spec = importlib.util.spec_from_file_location("eden_run", TOOLS / "eden-run.py")
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


def unset_names(arguments):
    prefix = "--property=UnsetEnvironment="
    return {name for argument in arguments if argument.startswith(prefix)
            for name in argument[len(prefix):].split()}


class Nx2EnvironmentTests(unittest.TestCase):
    def test_supplied_controls_override_manager_and_absent_controls_are_unset(self):
        supplied = {"EDEN_NX2_SPLIT": "0", "EDEN_NX2_GPU_PROFILE": "1"}
        arguments, recorded = systemd_nx2_environment(supplied)
        self.assertEqual(recorded, supplied)
        self.assertIn("--setenv=EDEN_NX2_SPLIT=0", arguments)
        self.assertIn("--setenv=EDEN_NX2_GPU_PROFILE=1", arguments)
        self.assertEqual(unset_names(arguments), set(NX2_ENVIRONMENT_VARIABLES) - supplied.keys())
        # Model systemd's final UnsetEnvironment step with stale manager state.
        effective = {"EDEN_NX2_SPLIT": "2", "EDEN_NX2_IPC_TRACE": "1"}
        effective.update(recorded)
        for name in unset_names(arguments):
            effective.pop(name, None)
        self.assertEqual(effective, supplied)

    def test_empty_and_zero_values_are_not_treated_as_absent(self):
        supplied = {"EDEN_NX2_IPC_TRACE": "", "EDEN_NX2_AUTO_STUB": "0"}
        arguments, recorded = systemd_nx2_environment(supplied)
        self.assertEqual(recorded, supplied)
        self.assertIn("--setenv=EDEN_NX2_IPC_TRACE=", arguments)
        self.assertIn("--setenv=EDEN_NX2_AUTO_STUB=0", arguments)
        self.assertTrue(supplied.keys().isdisjoint(unset_names(arguments)))

    def test_no_controls_clears_the_whole_known_set(self):
        arguments, recorded = systemd_nx2_environment({})
        self.assertEqual(recorded, {})
        self.assertEqual(unset_names(arguments), set(NX2_ENVIRONMENT_VARIABLES))
        self.assertFalse(any(arg.startswith("--setenv=") for arg in arguments))

    def test_unknown_secret_and_comment_only_names_are_not_exported_or_recorded(self):
        environment = {name: "must not escape" for name in (
            "EDEN_NX2_UNKNOWN", "EDEN_NX2_API_TOKEN", "EDEN_NX2_TC_",
            "EDEN_NX2_RT_TRACE_AFTER_UPSCALE", "PRIVATE_TOKEN", "PATH")}
        arguments, recorded = systemd_nx2_environment(environment)
        self.assertEqual(recorded, {})
        self.assertFalse(any(arg.startswith("--setenv=") for arg in arguments))
        self.assertFalse(environment.keys() & unset_names(arguments))

    def test_all_known_controls_can_be_supplied_without_unsetting_them(self):
        supplied = dict.fromkeys(NX2_ENVIRONMENT_VARIABLES, "1")
        arguments, recorded = systemd_nx2_environment(supplied)
        self.assertEqual(recorded, supplied)
        self.assertEqual(len(arguments), len(supplied))
        self.assertEqual(unset_names(arguments), set())

    def test_actual_launcher_passes_literal_arguments_and_records_only_controls(self):
        # Exercise main's real construction/save path while injecting process I/O.
        # Quotes, spaces, dollars, percent and '=' must stay inside one assignment.
        values = {
            "EDEN_NX2_FRAME_CAPTURE": '/tmp/frames space/"quoted"/$literal/100%/a=b',
            "EDEN_NX2_DUMP": "121:8100000000:1000,8200000000:2000",
            "EDEN_NX2_SPLIT": "0",
            "EDEN_NX2_IPC_TRACE": "",
        }
        environment = dict(values, EDEN_NX2_UNKNOWN="private", PRIVATE_TOKEN="secret")
        process = SimpleNamespace(stdout=io.BytesIO(), returncode=0,
                                  poll=lambda: 0, wait=lambda **kwargs: 0)
        manager = SimpleNamespace(returncode=0, stderr="",
                                  stdout="DISPLAY=:7\nEDEN_NX2_GPU_PROFILE=1\n")
        with tempfile.TemporaryDirectory(prefix="eden-nx2-env-") as temporary:
            output = Path(temporary) / "run"
            command = ["/tmp/eden path", "/tmp/title path/exefs/main"]
            with mock.patch.dict(os.environ, environment, clear=True), \
                 mock.patch.object(launcher.sys, "platform", "linux"), \
                 mock.patch.object(launcher, "bounded", return_value=manager), \
                 mock.patch.object(launcher, "diagnostics", return_value={"Result": "success"}), \
                 mock.patch.object(launcher.signal, "signal"), \
                 mock.patch.object(launcher.subprocess, "Popen", return_value=process) as popen, \
                 mock.patch("builtins.print"):
                result = launcher.main(["--output", str(output), "--", *command])
            self.assertEqual(result, 0)
            invocation = popen.call_args.args[0]
            for name, value in values.items():
                self.assertIn("--setenv=" + name + "=" + value, invocation)
            self.assertIn("EDEN_NX2_GPU_PROFILE", unset_names(invocation))
            self.assertNotIn("--setenv=EDEN_NX2_GPU_PROFILE=1", invocation)
            self.assertFalse(any("EDEN_NX2_UNKNOWN" in arg or "PRIVATE_TOKEN" in arg
                                 for arg in invocation))
            self.assertIn("--setenv=DISPLAY=:7", invocation)
            separator = invocation.index("--")
            self.assertEqual(invocation[separator + 1:separator + 3], command)
            self.assertNotIn("shell", popen.call_args.kwargs)
            manifest = json.loads((output / "run.json").read_text())
            self.assertEqual(manifest["nx2_environment"], values)
            self.assertEqual(manifest["command"], command)
            self.assertNotIn("secret", json.dumps(manifest))


if __name__ == "__main__":
    unittest.main()
