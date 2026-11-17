# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""CLI checks and opt-in tests against a real private Eden frontend.

CLI tests do not execute Eden. LdnControlLifecycle/LdnControlErrors require an
idle private frontend selected by EDEN_LDN_CONTROL_SOCKET and an actual IPv4
interface in EDEN_LDN_TEST_INTERFACE. They restore in-memory settings and stop
owned LDN listeners, but do not save the configuration or start/stop a game.
"""

import contextlib
import io
import json
import os
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
import edenctl


class LdnCli(unittest.TestCase):
    def invoke(self, arguments, response=None):
        if response is None:
            response = {"ok": True, "result": {"running": False}}
        output = io.StringIO()
        with mock.patch.object(edenctl.Client, "request", return_value=response) as request, \
                contextlib.redirect_stdout(output):
            code = edenctl.main(["--socket", "/unused/control.sock", *arguments])
        return code, json.loads(output.getvalue()), request

    def test_LdnControlLifecycle_commands(self):
        for operation in ("start", "stop", "status"):
            with self.subTest(operation=operation):
                code, response, request = self.invoke(["ldn", operation])
                self.assertEqual(code, 0)
                self.assertTrue(response["ok"])
                request.assert_called_once_with("ldn." + operation, {})

    def test_configure_interface_is_one_atomic_request(self):
        _, _, request = self.invoke(["ldn", "configure", "ldn_mitm", "--interface", "test nic"])
        request.assert_called_once_with("ldn.configure", {"backend": "ldn_mitm", "interface": "test nic"})

    def test_room_and_omitted_interface(self):
        _, _, request = self.invoke(["ldn", "configure", "room"])
        request.assert_called_once_with("ldn.configure", {"backend": "room"})

    def test_explicit_empty_interface_is_preserved(self):
        _, _, request = self.invoke(["ldn", "configure", "ldn_mitm", "--interface", ""])
        request.assert_called_once_with("ldn.configure", {"backend": "ldn_mitm", "interface": ""})

    def test_LdnControlErrors_preserve_server_error_and_exit_one(self):
        failure = {"ok": False, "error": {"code": "ldn_start_failed", "message": "address already in use"}}
        code, response, _ = self.invoke(["ldn", "start"], failure)
        self.assertEqual(code, 1)
        self.assertEqual(response, failure)

    def test_client_failure_exits_two(self):
        with mock.patch.object(edenctl.Client, "request", side_effect=OSError("socket absent")), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            code = edenctl.main(["--socket", "/unused/control.sock", "ldn", "status"])
        self.assertEqual(code, 2)
        self.assertEqual(json.loads(output.getvalue())["error"]["code"], "client_error")

    def test_invalid_backend_rejected_before_request(self):
        with mock.patch.object(edenctl.Client, "request") as request, \
                contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exited:
            edenctl.main(["--socket", "/unused/control.sock", "ldn", "configure", "unknown"])
        self.assertEqual(exited.exception.code, 2)
        request.assert_not_called()


@unittest.skipUnless(os.environ.get("EDEN_LDN_CONTROL_SOCKET") and os.environ.get("EDEN_LDN_TEST_INTERFACE"),
                     "requires an admitted private idle Eden frontend and real test interface")
class LdnControlLifecycle(unittest.TestCase):
    def call(self, method, **params):
        response = self.client.request(method, params)
        self.assertTrue(response["ok"], response)
        return response["result"]

    def setting(self, key):
        return self.call("settings.get", key="core." + key)["value"]

    def setUp(self):
        self.client = edenctl.Client(os.environ["EDEN_LDN_CONTROL_SOCKET"], timeout=10)
        self.assertFalse(self.call("status")["powered_on"], "use an idle private frontend")
        self.assertFalse(self.call("ldn.status")["running"], "frontend already owns LDN listeners")
        self.original = {key: self.setting(key) for key in
                         ("ldn_mitm", "network_interface", "airplane_mode", "lan_online")}
        self.interface = os.environ["EDEN_LDN_TEST_INTERFACE"]
        self.addCleanup(self.restore)

    def restore(self):
        self.call("ldn.stop")
        self.call("ldn.configure", backend="ldn_mitm" if self.original["ldn_mitm"] == "true" else "room",
                  interface=self.original["network_interface"])
        for key in ("airplane_mode", "lan_online"):
            self.assertEqual(self.setting(key), self.original[key], f"LDN changed {key}")

    def start(self):
        self.call("ldn.configure", backend="ldn_mitm", interface=self.interface)
        result = self.call("ldn.start")
        self.assertTrue(result["running"], result)
        self.assertEqual(result["active_backend"], "ldn_mitm")
        self.assertEqual(result["interface"], self.interface)
        self.assertNotIn(result["ip"], ("0.0.0.0", "255.255.255.255"))
        self.assertEqual(result["port"], 11452)
        for counter in ("received", "sent", "malformed"):
            self.assertTrue(result["counters"][counter].isdecimal())
        return result

    def test_LdnControlLifecycle_start_stop_restart_and_room(self):
        schema = self.call("schema")
        for method in ("ldn.configure", "ldn.start", "ldn.stop", "ldn.status"):
            self.assertIn(method, schema["methods"])
            self.assertIn(method, schema["params"])
        selected = self.call("ldn.configure", backend="ldn_mitm", interface=self.interface)
        self.assertEqual(selected["configured_backend"], "ldn_mitm")
        self.assertFalse(selected["running"])
        self.assertEqual(selected["active_backend"], "none")
        first = self.start()
        again = self.call("ldn.start")
        self.assertTrue(again["running"])
        self.assertEqual(again["ip"], first["ip"])
        self.assertEqual(again["peers"], first["peers"])
        self.assertFalse(self.call("ldn.stop")["running"])
        self.assertFalse(self.call("ldn.stop")["running"])
        self.start()
        self.assertFalse(self.call("ldn.configure", backend="room")["running"])
        error = self.client.request("ldn.start")["error"]
        self.assertEqual(error["code"], "ldn_not_selected")

    def test_LdnControlErrors_invalid_params_are_atomic(self):
        self.start()
        before = {key: self.setting(key) for key in self.original}
        cases = ({}, {"backend": "other"}, {"backend": False},
                 {"backend": "room", "interface": 12}, {"backend": "room", "interface": None},
                 {"backend": "room", "extra": True})
        for params in cases:
            with self.subTest(params=params):
                response = self.client.request("ldn.configure", params)
                self.assertEqual(response["error"]["code"], "invalid_params")
                self.assertEqual({key: self.setting(key) for key in before}, before)
                self.assertTrue(self.call("ldn.status")["running"])
        for method in ("ldn.start", "ldn.stop", "ldn.status"):
            self.assertEqual(self.client.request(method, {"unexpected": True})["error"]["code"], "invalid_params")
        self.assertTrue(self.call("ldn.status")["running"])

    def test_generic_settings_metadata_validation_and_listener_stop(self):
        entries = self.call("settings.list")["settings"]
        setting = next(item for item in entries if item["key"] == "core.ldn_mitm")
        self.assertFalse(setting["runtime_modifiable"])
        self.assertTrue(setting["persistent"])
        self.assertEqual(setting["default"], "false")
        self.start()
        response = self.client.request("settings.set", {"key": "core.ldn_mitm", "value": "false"})
        self.assertEqual(response["error"]["code"], "invalid_value")
        self.assertTrue(self.call("ldn.status")["running"])
        response = self.client.request("settings.set", {"key": "core.network_interface", "value": False})
        self.assertEqual(response["error"]["code"], "invalid_value")
        self.assertTrue(self.call("ldn.status")["running"])
        self.call("settings.set", key="core.ldn_mitm", value=False)
        self.assertFalse(self.call("ldn.status")["running"])
        self.start()
        self.call("settings.set", key="core.network_interface", value="__eden_ldn_missing_interface__")
        self.assertFalse(self.call("ldn.status")["running"])
        failed = self.client.request("ldn.start")
        self.assertEqual(failed["error"]["code"], "invalid_configuration")
        self.assertTrue(failed["error"]["message"])


@unittest.skipUnless(os.environ.get("EDEN_LDN_POWERED_CONTROL_SOCKET"),
                     "requires a separately admitted frontend with a game already loaded")
class LdnPoweredLock(unittest.TestCase):
    def test_LdnControlErrors_existing_powered_on_setting_lock(self):
        client = edenctl.Client(os.environ["EDEN_LDN_POWERED_CONTROL_SOCKET"])
        status = client.request("status")
        self.assertTrue(status["ok"] and status["result"]["powered_on"])
        before = client.request("ldn.status")["result"]
        response = client.request("ldn.configure", {"backend": before["configured_backend"]})
        self.assertEqual(response["error"]["code"], "not_runtime_modifiable")
        for key, value in (("ldn_mitm", before["configured_backend"] == "ldn_mitm"),
                           ("network_interface", before["configured_interface"])):
            response = client.request("settings.set", {"key": "core." + key, "value": value})
            self.assertEqual(response["error"]["code"], "not_runtime_modifiable")
        self.assertEqual(client.request("ldn.status")["result"]["configured_backend"], before["configured_backend"])


if __name__ == "__main__":
    unittest.main()
