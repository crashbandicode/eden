#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Attach to an opt-in Eden control socket using only the Python standard library."""

import argparse
import json
import math
import os
import socket
import sys
import time
import uuid


MAX_RESPONSE = 16 * 1024 * 1024


class ControlError(Exception):
    pass


class Client:
    def __init__(self, path, timeout=5.0):
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("timeout must be finite and positive")
        self.path = os.fspath(path)
        self.timeout = timeout

    def request(self, method, params=None):
        request_id = uuid.uuid4().hex
        request = {"id": request_id, "method": method, "params": params or {}}
        payload = json.dumps(request, allow_nan=False, separators=(",", ":")).encode() + b"\n"
        if len(payload) > 1024 * 1024:
            raise ControlError("request exceeds 1 MiB")
        deadline = time.monotonic() + self.timeout
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(self.timeout)
            sock.connect(self.path)
            sock.sendall(payload)
            data = bytearray()
            while b"\n" not in data:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("control request deadline exceeded")
                sock.settimeout(remaining)
                chunk = sock.recv(min(65536, MAX_RESPONSE + 1 - len(data)))
                if not chunk:
                    raise ControlError("Eden disconnected before replying")
                data.extend(chunk)
                if len(data) > MAX_RESPONSE:
                    raise ControlError("response exceeds 16 MiB")
        response = json.loads(data.split(b"\n", 1)[0])
        if not isinstance(response, dict) or response.get("id") != request_id:
            raise ControlError("invalid response or mismatched request ID")
        return response


def print_json(value):
    print(json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":")), flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", default=os.environ.get("EDEN_CONTROL_SOCKET"),
                        help="absolute socket path passed to Eden --control-socket")
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="total deadline for each request in seconds (default: 5)")
    parser.add_argument("--wait", type=float, default=0,
                        help="wait up to N seconds for a returned job; modal dialogs may need another client")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("ping", "status", "schema", "actions", "tree", "settings"):
        commands.add_parser(name)
    call = commands.add_parser("call", help="invoke any discovered protocol method")
    call.add_argument("method")
    call.add_argument("params", nargs="?", default="{}", help="JSON object, or - to read stdin")
    commands.add_parser("attach", help="read JSON requests from stdin; one JSON response per line")
    recording = commands.add_parser("recording", help="record game frames to an animated GIF")
    record_commands = recording.add_subparsers(dest="record_command", required=True)
    record_start = record_commands.add_parser("start")
    record_start.add_argument("path", help="absolute .gif path on the Eden host (must not exist)")
    record_start.add_argument("--fps", type=int, default=10)
    record_start.add_argument("--width", type=int, default=640)
    record_start.add_argument("--duration-ms", type=int, default=30000)
    record_commands.add_parser("stop")
    record_commands.add_parser("status")
    macro = commands.add_parser("macro", help="record, branch, and play named input macros")
    macro_commands = macro.add_subparsers(dest="macro_command", required=True)
    for name in ("list", "status", "stop"):
        macro_commands.add_parser(name)
    for name in ("play", "inspect", "record-from", "bind"):
        sub = macro_commands.add_parser(name)
        source = sub.add_mutually_exclusive_group(required=True)
        source.add_argument("--name", help="saved macro name")
        source.add_argument("--path", help="absolute macro file path on the Eden host")
        if name in ("play", "record-from"):
            sub.add_argument("--player", type=int, choices=range(10))
        if name == "record-from":
            sub.add_argument("--marker", required=True)
            sub.add_argument("--new-name", required=True)
            sub.add_argument("--output-path")
            sub.add_argument("--countdown-ms", type=int, default=3000)
        if name == "bind":
            sub.add_argument("--keyboard", default="", help="e.g. Ctrl+Alt+1; empty clears")
            sub.add_argument("--gamepad", default="", help="e.g. l+r+plus; empty clears")
    take = macro_commands.add_parser("record")
    take.add_argument("name")
    take.add_argument("--path")
    take.add_argument("--player", type=int, choices=range(10), default=0)
    take.add_argument("--countdown-ms", type=int, default=3000)
    marker = macro_commands.add_parser("marker")
    marker.add_argument("--name")
    shortcuts = macro_commands.add_parser("shortcuts")
    shortcuts.add_argument("--marker-keyboard")
    shortcuts.add_argument("--marker-gamepad")
    screenshot = commands.add_parser("screenshot", help="capture a GUI window or screen")
    screenshot.add_argument("path")
    screenshot.add_argument("--target", default="active")
    trigger = commands.add_parser("trigger", help="trigger a discovered QAction")
    trigger.add_argument("target")
    click = commands.add_parser("click", help="click a discovered widget")
    click.add_argument("target")
    set_value = commands.add_parser("set", help="set a discovered widget property")
    set_value.add_argument("target")
    set_value.add_argument("property")
    set_value.add_argument("value", help="JSON value, e.g. true, 42, or '\"text\"'")
    button = commands.add_parser("button")
    button.add_argument("button")
    button.add_argument("--player", type=int, default=0)
    button.add_argument("--state", choices=("press", "release", "tap"), default="tap")
    button.add_argument("--duration-ms", type=int, default=100)
    stick = commands.add_parser("stick")
    stick.add_argument("stick", choices=("left", "right"))
    stick.add_argument("x", type=float)
    stick.add_argument("y", type=float)
    stick.add_argument("--player", type=int, default=0)
    commands.add_parser("release", help="release every injected controller/touch input")
    motion = commands.add_parser("motion", help="send gyro radians/s and acceleration in G")
    motion.add_argument("--player", type=int, default=0)
    motion.add_argument("--sensor", choices=("both", "left", "right"), default="both")
    motion.add_argument("--gyro", type=float, nargs=3, required=True, metavar=("X", "Y", "Z"))
    motion.add_argument("--accel", type=float, nargs=3, required=True, metavar=("X", "Y", "Z"))
    motion.add_argument("--delta-us", type=int, default=10000)
    touch = commands.add_parser("touch", help="send normalized touchscreen coordinates")
    touch.add_argument("state", choices=("press", "move", "release"))
    touch.add_argument("--finger", type=int, default=0)
    touch.add_argument("--x", type=float)
    touch.add_argument("--y", type=float)
    analog_trigger = commands.add_parser("analog-trigger", help="send a GameCube analog trigger")
    analog_trigger.add_argument("trigger", choices=("left", "right"))
    analog_trigger.add_argument("value", type=float)
    analog_trigger.add_argument("--player", type=int, default=0)
    ring = commands.add_parser("ring", help="send Ring-Con flex in [-1,1]")
    ring.add_argument("value", type=float)
    ring.add_argument("--player", type=int, default=0)
    for input_parser in (button, stick, motion, touch, analog_trigger, ring):
        input_parser.add_argument("--lease-ms", type=int, default=1000)
    args = parser.parse_args(argv)
    if not args.socket:
        parser.error("--socket or EDEN_CONTROL_SOCKET is required")
    try:
        if not math.isfinite(args.wait) or args.wait < 0:
            raise ValueError("--wait must be finite and nonnegative")
        client = Client(args.socket, args.timeout)
        if args.command == "attach":
            failed = False
            for line in sys.stdin:
                if not line.strip():
                    continue
                try:
                    request = json.loads(line)
                    if not isinstance(request, dict) or not isinstance(request.get("method"), str):
                        raise ValueError("expected an object with a method string")
                    params = request.get("params", {})
                    if not isinstance(params, dict):
                        raise ValueError("params must be an object")
                    response = client.request(request["method"], params)
                    if "id" in request:
                        response["id"] = request["id"]
                except (OSError, ValueError, ControlError) as error:
                    response = {"ok": False, "error": {"code": "client_error", "message": str(error)}}
                print_json(response)
                failed |= not response.get("ok", False)
            return int(failed)
        method = {"actions": "ui.actions", "tree": "ui.tree", "settings": "settings.list",
                  "release": "input.reset"}.get(args.command, args.command)
        params = {}
        if args.command == "call":
            method = args.method
            params = json.loads(sys.stdin.read() if args.params == "-" else args.params)
            if not isinstance(params, dict):
                raise ValueError("params must be a JSON object")
        elif args.command == "recording":
            method = "recording." + args.record_command
            if args.record_command == "start":
                params = {"path": args.path, "fps": args.fps, "width": args.width,
                          "duration_ms": args.duration_ms}
        elif args.command == "macro":
            method = "macro." + args.macro_command.replace("-", "_")
            for key in ("name", "path", "player", "countdown_ms", "marker", "new_name",
                        "output_path", "keyboard", "gamepad", "marker_keyboard", "marker_gamepad"):
                value = getattr(args, key, None)
                if value is not None:
                    params[key] = value
        elif args.command == "screenshot":
            method, params = "ui.screenshot", {"path": os.path.abspath(args.path), "target": args.target}
        elif args.command in ("trigger", "click"):
            method, params = "ui." + args.command, {"target": args.target}
        elif args.command == "set":
            method, params = "ui.set", {"target": args.target, "property": args.property,
                                       "value": json.loads(args.value)}
        elif args.command == "button":
            method, params = "input.button", {"button": args.button, "player": args.player,
                                              "state": args.state, "duration_ms": args.duration_ms}
        elif args.command == "stick":
            method, params = "input.stick", {"stick": args.stick, "player": args.player,
                                             "x": args.x, "y": args.y}
        elif args.command == "motion":
            method, params = "input.motion", {"player": args.player, "gyro": args.gyro,
                                               "accel": args.accel, "delta_us": args.delta_us,
                                               "sensor": args.sensor}
        elif args.command == "analog-trigger":
            method, params = "input.trigger", {"player": args.player, "trigger": args.trigger,
                                                "value": args.value}
        elif args.command == "ring":
            method, params = "input.ring", {"player": args.player, "value": args.value}
        elif args.command == "touch":
            method, params = "input.touch", {"finger": args.finger, "state": args.state}
            if args.x is not None:
                params["x"] = args.x
            if args.y is not None:
                params["y"] = args.y
        if args.command in ("button", "stick", "motion", "touch", "analog-trigger", "ring"):
            params["lease_ms"] = args.lease_ms
        response = client.request(method, params)
        if args.wait and response.get("ok") and isinstance(response.get("result"), dict):
            job = response["result"].get("job")
            deadline = time.monotonic() + args.wait
            while job:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    response = {"ok": False, "error": {"code": "job_timeout", "message": "Job is still pending; inspect jobs.get and ui.tree", "job": job}}
                    break
                waiter = Client(args.socket, min(args.timeout, remaining))
                progress = waiter.request("jobs.get", {"job": job})
                if not progress.get("ok"):
                    response = progress
                    break
                result = progress["result"]
                if result["state"] in ("done", "error"):
                    response = result["response"]
                    response["job"] = job
                    break
                time.sleep(min(0.05, max(0, deadline - time.monotonic())))
        print_json(response)
        return 0 if response.get("ok") else 1
    except (OSError, ValueError, ControlError) as error:
        print_json({"ok": False, "error": {"code": "client_error", "message": str(error)}})
        return 2


if __name__ == "__main__":
    sys.exit(main())
