# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Protocol and real Qt lifecycle tests; EDEN_CONTROL_FIXTURE selects the built fixture."""

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
from edenctl import Client

FIXTURE_ENV = dict(os.environ, QT_QPA_PLATFORM="offscreen", EDEN_CONTROL_IDLE_TIMEOUT_MS="1000")


class LiveControl(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="eden-control-test-")
        self.root = Path(self.directory.name)
        self.path = self.root / "control.sock"
        self.stderr = (self.root / "stderr").open("wb")
        env = dict(FIXTURE_ENV)
        if self._testMethodName == "test_composite_at_200_percent_matches_window_pixels":
            env["QT_SCALE_FACTOR"] = "2"
            env["QT_QPA_PLATFORM"] = "xcb"
        self.process = subprocess.Popen([os.environ["EDEN_CONTROL_FIXTURE"], "--control-socket", str(self.path),
                                         "--control-log", str(self.root / "control.jsonl")], env=env,
                                        stdout=self.stderr, stderr=subprocess.STDOUT)
        self.addCleanup(self.cleanup)
        self.client = Client(self.path, timeout=2)
        self.until(lambda: self.client.request("ping")["ok"])

    def cleanup(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)
        self.stderr.close()
        self.directory.cleanup()

    def until(self, predicate, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                result = predicate()
                if result:
                    return result
            except (OSError, KeyError, StopIteration):
                pass
            time.sleep(0.02)
        self.fail("condition was not reached before deadline")

    def call(self, method, **params):
        response = self.client.request(method, params)
        self.assertTrue(response["ok"], response)
        return response["result"]

    def finished(self, job):
        return self.until(lambda: (j if (j := self.call("jobs.get", job=job))["state"] in ("done", "error") else None))

    def job(self, method, **params):
        accepted = self.call(method, **params)
        self.assertTrue(accepted["accepted"])
        result = self.finished(accepted["job"])
        self.assertEqual(result["state"], "done", result)
        return result["response"]["result"]

    def failed_job(self, method, **params):
        result = self.finished(self.call(method, **params)["job"])
        self.assertEqual(result["state"], "error", result)
        return result["response"]["error"]["code"]

    def note(self):
        return self.call("fixture.note")["text"]

    def named(self, method, name, **params):
        return next(o for o in self.call(method, **params) if o["name"] == name)

    def test_discovery_and_options(self):
        tree = self.call("ui.tree")
        self.assertTrue(any(o["name"] == "volume" for o in tree))
        self.assertTrue(any(o["name"] == "configure" for o in self.call("ui.actions")))
        self.assertEqual(self.job("ui.set", target="volume", property="value", value=42)["value"], 42)
        self.assertEqual(self.job("ui.set", target="filename", property="text", value="game.nsp")["value"], "game.nsp")
        self.assertEqual(self.job("ui.set", target="renderer", property="currentIndex", value=1)["value"], 1)

    def test_modal_reentrancy_and_destroyed_id(self):
        accepted = self.call("ui.trigger", target="configure")
        dialog = self.until(lambda: next((o for o in self.call("ui.tree") if o["name"] == "configuration"), None))
        self.assertTrue(self.call("ping")["ready"])
        self.assertEqual(self.call("jobs.get", job=accepted["job"])["state"], "running")
        blocked = self.client.request("ui.set", {"target": "volume", "property": "value", "value": 10})
        self.assertFalse(blocked["ok"])
        self.assertEqual(blocked["error"]["code"], "modal_active")
        self.job("ui.click", target="accept")
        self.until(lambda: self.call("jobs.get", job=accepted["job"])["state"] == "done")
        missing = self.client.request("ui.close", {"target": dialog["id"]})
        self.assertFalse(missing["ok"])

    def test_disabled_control(self):
        result = self.client.request("ui.click", {"target": "disabled"})
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "disabled")

    def test_screenshot_reports_real_dimensions(self):
        path = self.root / "capture.png"
        result = self.call("ui.screenshot", target="main", path=str(path))
        self.assertGreater(result["width"], 100)
        self.assertGreater(result["height"], 100)
        self.assertEqual(path.read_bytes()[:8], b"\x89PNG\r\n\x1a\n")

    def test_item_model_edit_and_bounds(self):
        self.assertEqual(self.call("ui.items", target="games")["rows"], 1)
        self.job("ui.select", target="games", row=0, check_state=2)
        self.assertEqual(self.call("ui.items", target="games")["items"][0]["columns"][0]["check_state"], 2)
        self.job("ui.select", target="games", row=0, column=1, value="2.0")
        self.assertEqual(self.call("ui.items", target="games")["items"][0]["columns"][1]["text"], "2.0")
        result = self.client.request("ui.items", {"target": "games", "limit": -1})
        self.assertFalse(result["ok"])

    def test_fragmented_pipeline_and_invalid_json(self):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(2)
            sock.connect(str(self.path))
            sock.sendall(b'{"id":1,"method":"pi')
            sock.sendall(b'ng"}\nnot json\n{"id":2,"method":"ping"}\n')
            stream = sock.makefile("rb")
            responses = [json.loads(stream.readline()) for _ in range(3)]
            self.assertEqual(responses[0]["id"], 1)
            self.assertEqual(responses[1]["error"]["code"], "invalid_json")
            self.assertEqual(responses[2]["id"], 2)
            stream.close()

    def test_shutdown_with_attached_client(self):
        # A socket still owned by QLocalServer used to emit disconnected after the
        # ControlServer buffer map had already been destroyed.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as attached:
            attached.settimeout(2)
            attached.connect(str(self.path))
            attached.sendall(b'{"id":1,"method":"ping"}\n')
            self.assertIn(b'"ok":true', attached.recv(4096))
            self.job("ui.close", target="secondary")
            self.call("ui.close", target="main")
            self.assertEqual(self.process.wait(timeout=3), 0)

    def test_oversized_request_does_not_hang_gui(self):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(2)
            sock.connect(str(self.path))
            sock.sendall(b"x" * (1024 * 1024 + 1))
            response = json.loads(sock.makefile("rb").readline())
            self.assertEqual(response["error"]["code"], "request_too_large")
        self.assertTrue(self.call("ping")["ready"])

    def test_invalid_property_type_is_reported_in_job(self):
        self.assertEqual(self.failed_job("ui.set", target="volume", property="value", value="bad"), "invalid_value")

    # Popup menus: the item must be activated through QMenu's keyboard path so the menu closes
    # before the handler runs, exactly as a user's Return key does.
    def test_popup_action_follows_keyboard_path(self):
        context = self.call("ui.context", target="games", row=0)
        found = self.until(lambda: self.named("ui.actions", "ctx_configure"))
        self.assertTrue(found["reachable"])
        self.assertTrue(self.call("ping")["popup"])
        self.assertEqual(self.call("jobs.get", job=context["job"])["state"], "running")
        self.assertEqual(self.job("ui.trigger", target="ctx_configure")["path"], "popup_key")
        self.until(lambda: self.call("jobs.get", job=context["job"])["state"] == "done")
        self.assertEqual(self.note(), "ctx_configure fired menu_visible=0 | menu closed")
        self.assertFalse(self.call("ping")["popup"])

    def test_disabled_popup_item_is_refused_and_menu_can_be_closed(self):
        context = self.call("ui.context", target="games", row=0)
        self.until(lambda: self.named("ui.actions", "ctx_disabled"))
        self.assertEqual(self.failed_job("ui.trigger", target="ctx_disabled"), "unavailable_action")
        self.job("ui.close", target="ctx_menu")
        self.until(lambda: self.call("jobs.get", job=context["job"])["state"] == "done")
        self.assertNotIn("disabled fired", self.note())

    def test_popup_inside_application_modal_dialog_is_reachable(self):
        configure = self.call("ui.trigger", target="configure")
        self.until(lambda: self.named("ui.tree", "dialog_games", brief=True))
        context = self.call("ui.context", target="dialog_games", row=0)
        found = self.until(lambda: self.named("ui.actions", "dlg_configure"))
        self.assertTrue(found["reachable"])
        self.assertEqual(self.job("ui.trigger", target="dlg_configure")["path"], "popup_key")
        self.until(lambda: self.call("jobs.get", job=context["job"])["state"] == "done")
        self.assertEqual(self.note(), "dlg_configure fired menu_visible=0 | menu closed")
        self.job("ui.click", target="accept")
        self.until(lambda: self.call("jobs.get", job=configure["job"])["state"] == "done")

    def test_menubar_menu_opens_through_its_title(self):
        title = next(o for o in self.call("ui.actions") if o.get("menu") and o["text"] == "Test")
        self.assertEqual(self.job("ui.trigger", target=title["id"])["path"], "menubar")
        self.until(lambda: self.call("ping")["popup"])
        self.assertTrue(self.named("ui.actions", "configure")["reachable"])
        self.job("ui.close", target="test_menu")
        self.until(lambda: not self.call("ping")["popup"])

    # Hotkey-only commands (QShortcut without a QAction) are discoverable and triggerable.
    def test_shortcut_discovery_and_trigger(self):
        shortcut = self.named("ui.actions", "bare_shortcut")
        self.assertEqual(shortcut["class"], "QShortcut")
        self.assertEqual(shortcut["shortcut"], "F10")
        self.assertTrue(shortcut["enabled"] and shortcut["visible"] and shortcut["reachable"])
        self.assertEqual(self.job("ui.trigger", target="bare_shortcut")["path"], "shortcut")
        self.assertEqual(self.note(), "bare_shortcut fired")
        self.assertEqual(self.failed_job("ui.trigger", target="disabled_shortcut"), "unavailable_shortcut")
        self.job("ui.trigger", target="fullscreen")
        self.assertEqual(self.note(), "fullscreen triggered=1")
        self.job("ui.key", target="main", key="F10")
        self.assertEqual(self.note(), "bare_shortcut fired")

    def test_action_checked_requires_trigger(self):
        self.assertEqual(self.failed_job("ui.set", target="fullscreen", property="checked", value=True), "read_only")
        self.assertEqual(self.job("ui.trigger", target="fullscreen")["path"], "trigger")
        self.assertEqual(self.note(), "fullscreen triggered=1")
        self.assertTrue(self.named("ui.actions", "fullscreen")["checked"])

    # Modality: a window-modal dialog blocks its parent chain only; unrelated windows stay usable.
    def test_window_modal_blocks_only_its_parent_chain(self):
        accepted = self.call("ui.trigger", target="window_modal")
        self.until(lambda: self.named("ui.tree", "window_modal_dialog", brief=True))
        blocked = self.client.request("ui.set", {"target": "volume", "property": "value", "value": 3})
        self.assertEqual(blocked["error"]["code"], "modal_active")
        self.assertFalse(self.named("ui.tree", "volume", brief=True)["reachable"])
        self.assertTrue(self.named("ui.tree", "secondary_button", brief=True)["reachable"])
        self.job("ui.click", target="secondary_button")
        self.assertEqual(self.note(), "secondary clicked")
        self.job("ui.click", target="window_modal_accept")
        self.until(lambda: self.call("jobs.get", job=accepted["job"])["state"] == "done")
        self.assertEqual(self.job("ui.set", target="volume", property="value", value=3)["value"], 3)

    def test_application_modal_blocks_other_windows(self):
        accepted = self.call("ui.trigger", target="configure")
        self.until(lambda: self.named("ui.tree", "configuration", brief=True))
        blocked = self.client.request("ui.click", {"target": "secondary_button"})
        self.assertEqual(blocked["error"]["code"], "modal_active")
        self.job("ui.click", target="accept")
        self.until(lambda: self.call("jobs.get", job=accepted["job"])["state"] == "done")

    # Screenshots: the default target must work when no window has keyboard focus, and child
    # captures must come from the already-native window rather than making the child native.
    def test_active_target_falls_back_to_a_visible_window(self):
        self.assertFalse(self.call("fixture.deactivate")["active_window"])
        result = self.call("ui.screenshot", path=str(self.root / "active.png"))
        self.assertGreater(result["width"], 100)
        self.assertEqual(result["target"], self.named("ui.tree", "main", brief=True)["id"])
        self.assertTrue(any(o["name"] == "main" for o in self.call("ui.tree", target="active", brief=True)))

    def test_child_screenshot_is_cropped_from_its_window(self):
        whole = self.call("ui.screenshot", target="main", path=str(self.root / "main.png"))
        child = self.call("ui.screenshot", target="volume", path=str(self.root / "volume.png"))
        self.assertGreater(child["width"], 10)
        self.assertLess(child["width"], whole["width"])
        self.assertLess(child["height"], 100)

    # A screenshot must say where its pixels came from and never pass off a blank image as a capture.
    def test_screenshot_reports_backend_and_uniformity(self):
        result = self.call("ui.screenshot", target="main", path=str(self.root / "report.png"))
        self.assertIn(result["backend"], ("x11-window", "screen-window", "widget-render"))
        self.assertFalse(result["uniform"], result)
        status = self.call("ui.screenshot", target="status", path=str(self.root / "status.png"))
        self.assertFalse(status["uniform"], status)
        self.assertEqual(status["target"], self.named("ui.tree", "status", brief=True)["id"])

    def test_screenshot_all_composites_every_visible_window(self):
        accepted = self.call("ui.trigger", target="configure")
        self.until(lambda: self.named("ui.tree", "configuration", brief=True))
        result = self.call("ui.screenshot", target="all", path=str(self.root / "all.png"))
        self.assertEqual(result["backend"], "composite")
        self.assertFalse(result["uniform"])
        classes = {w["class"] for w in result["windows"]}
        self.assertTrue({"QMainWindow", "QDialog"} <= classes, classes)
        self.assertTrue(all(not w["uniform"] for w in result["windows"]), result["windows"])
        self.assertGreaterEqual(result["width"], max(w["geometry"][2] for w in result["windows"]))
        self.job("ui.click", target="accept")
        self.until(lambda: self.call("jobs.get", job=accepted["job"])["state"] == "done")

    @unittest.skipUnless(os.environ.get("EDEN_CONTROL_TEST_XCB") == "1" and os.environ.get("DISPLAY"),
                         "200% capture regression requires an X11 DISPLAY")
    def test_composite_at_200_percent_matches_window_pixels(self):
        try:
            from PIL import Image, ImageChops
        except ImportError:
            self.skipTest("PIL is required to inspect pixels")

        self.job("ui.close", target="secondary")
        whole_path = self.root / "whole-2x.png"
        all_path = self.root / "all-2x.png"
        whole = self.until(lambda: (r if (r := self.call("ui.screenshot", target="main", path=str(whole_path)))["backend"] == "x11-window" and not r["uniform"] else None), timeout=10)
        composite = self.call("ui.screenshot", target="all", path=str(all_path))
        self.assertGreater(whole["width"], 1000)
        self.assertEqual((whole["width"], whole["height"]),
                         (composite["width"], composite["height"]))

        def status_bounds(path):
            with Image.open(path) as image:
                red, green, blue = image.convert("RGB").split()
                mask = ImageChops.multiply(red.point(lambda v: 255 if v > 200 else 0),
                                          blue.point(lambda v: 255 if v > 200 else 0))
                return ImageChops.multiply(mask, green.point(lambda v: 255 if v < 60 else 0)).getbbox()

        expected = status_bounds(whole_path)
        self.assertIsNotNone(expected)
        self.assertEqual(status_bounds(all_path), expected)

    def test_tree_filters_and_schema(self):
        brief = self.call("ui.tree", brief=True)
        self.assertTrue(brief and all("properties" not in o for o in brief))
        spins = self.call("ui.tree", **{"class": "QSpinBox"})
        self.assertTrue(spins and all(o["class"] == "QSpinBox" for o in spins))
        self.assertTrue(all(o["visible"] for o in self.call("ui.tree", visible_only=True, brief=True) if "visible" in o))
        schema = self.call("schema")
        for method in ("ui.trigger", "ui.screenshot", "input.state", "jobs.get"):
            self.assertIn(method, schema["methods"])
        self.assertIn("ui.trigger", schema["params"])
        self.assertIn("modal_active", schema["errors"])
        self.assertGreaterEqual(schema["limits"]["connections"], 16)
        self.assertEqual(schema["limits"]["idle_timeout_ms"], 1000)

    # Transport bounds: idle connections are reaped and cannot starve the watchdog's ping.
    def test_idle_connections_are_reaped_and_do_not_starve_pings(self):
        idle = []
        for _ in range(40):
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(2)
            sock.connect(str(self.path))
            idle.append(sock)
        try:
            self.until(lambda: self.client.request("ping")["ok"], timeout=4)

            def all_closed():
                for sock in idle:
                    sock.settimeout(0.05)
                    try:
                        if sock.recv(1) != b"":
                            return False
                    except socket.timeout:
                        return False
                    except OSError:
                        pass
                return True
            self.until(all_closed, timeout=4)
        finally:
            for sock in idle:
                sock.close()
        self.assertTrue(self.call("ping")["ready"])

    # Socket ownership: a live endpoint is never replaced; a dead socket file is reclaimed.
    def test_second_server_refuses_live_socket_and_reclaims_stale(self):
        second = subprocess.run([os.environ["EDEN_CONTROL_FIXTURE"], "--control-socket", str(self.path)],
                                env=FIXTURE_ENV, capture_output=True, text=True, timeout=15)
        self.assertEqual(second.returncode, 2, second.stderr)
        self.assertIn("socket_in_use", second.stderr)
        self.assertEqual(self.call("ping")["pid"], self.process.pid)
        self.process.kill()
        self.process.wait(timeout=5)
        self.assertTrue(self.path.exists())
        third = subprocess.Popen([os.environ["EDEN_CONTROL_FIXTURE"], "--control-socket", str(self.path)],
                                 env=FIXTURE_ENV, stdout=self.stderr, stderr=subprocess.STDOUT)
        try:
            self.assertEqual(self.until(lambda: self.client.request("ping"))["result"]["pid"], third.pid)
        finally:
            third.kill()
            third.wait(timeout=5)


@unittest.skipUnless(os.environ.get("EDEN_CONTROL_TEST_XCB") == "1" and os.environ.get("DISPLAY"),
                     "set EDEN_CONTROL_TEST_XCB=1 with an X11 DISPLAY to test real window capture")
class XcbCapture(LiveControl):
    """Real X server (also Xwayland): pixels must come from the window itself, not the root window.

    Qt's QScreen::grabWindow copies from the root window whenever the target shares the root's
    depth; under Xwayland that root is empty and every grab is black. The fixture window appears on
    the desktop for the duration of the test.
    """

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="eden-control-xcb-")
        self.root = Path(self.directory.name)
        self.path = self.root / "control.sock"
        self.stderr = (self.root / "stderr").open("wb")
        env = dict(FIXTURE_ENV, QT_QPA_PLATFORM="xcb")
        self.process = subprocess.Popen([os.environ["EDEN_CONTROL_FIXTURE"], "--control-socket", str(self.path)],
                                        env=env, stdout=self.stderr, stderr=subprocess.STDOUT)
        self.addCleanup(self.cleanup)
        self.client = Client(self.path, timeout=2)
        self.until(lambda: self.client.request("ping")["ok"], timeout=10)

    def test_window_pixels_are_real(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("PIL is required to inspect pixels")
        path = self.root / "status.png"
        # Wait until the window is mapped and painted; the first frames after map can be empty.
        result = self.until(lambda: (r if (r := self.call("ui.screenshot", target="status", path=str(path)))["backend"] == "x11-window" and not r["uniform"] else None), timeout=10)
        self.assertEqual(result["backend"], "x11-window", result)
        self.assertNotIn("warning", result)
        with Image.open(path) as image:
            pixel = image.convert("RGB").getpixel((image.width // 2, image.height // 2))
        self.assertGreater(pixel[0], 200, pixel)
        self.assertLess(pixel[1], 60, pixel)
        self.assertGreater(pixel[2], 200, pixel)
        whole = self.call("ui.screenshot", target="main", path=str(self.root / "main.png"))
        self.assertEqual(whole["backend"], "x11-window")
        self.assertFalse(whole["uniform"])
        self.assertGreater(whole["width"], 500)

    # Inherited offscreen tests are not meaningful on a shared desktop; run only the xcb check.
    def run(self, result=None):
        if self._testMethodName != "test_window_pixels_are_real":
            return result
        return super().run(result)


if __name__ == "__main__":
    unittest.main()
