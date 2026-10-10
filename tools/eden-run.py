#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run Eden in a Linux user cgroup with independent time and responsiveness limits."""

import argparse
import json
import logging
import logging.handlers
import math
import os
import re
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import threading
import time
import uuid

from edenctl import Client, ControlError


def bounded(command, timeout=5):
    return subprocess.run(command, capture_output=True, text=True, timeout=timeout, check=False)


def save(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def duration_seconds(value):
    scales = {"us": 1e-6, "ms": 1e-3, "s": 1, "min": 60, "h": 3600, "d": 86400}
    parts = re.findall(r"([0-9]+(?:\.[0-9]+)?)(us|ms|min|s|h|d)", value or "")
    return sum(float(number) * scales[unit] for number, unit in parts) if parts else None


def stop_unit(unit):
    try:
        result = bounded(["systemctl", "--user", "stop", unit], timeout=10)
        if result.returncode == 0:
            return
    except subprocess.TimeoutExpired:
        pass
    bounded(["systemctl", "--user", "kill", "--kill-whom=all", "--signal=KILL", unit])


def diagnostics(unit, directory):
    try:
        result = bounded(["systemctl", "--user", "show", unit,
                          "--property=MainPID,ActiveState,SubState,Result,ExecMainCode,ExecMainStatus,"
                          "MemoryCurrent,MemoryPeak,MemoryMax,MemorySwapMax,RuntimeMaxUSec,ControlGroup"])
        (directory / "unit.txt").write_text(result.stdout + result.stderr)
        props = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
        cgroup = props.get("ControlGroup", "")
        if cgroup.startswith("/"):
            for name in ("memory.events", "memory.peak", "cgroup.events"):
                try:
                    contents = (Path("/sys/fs/cgroup") / cgroup.lstrip("/") / name).read_text()
                    (directory / name).write_text(contents)
                    props[name] = contents.strip()
                except OSError:
                    pass
        pid = int(props.get("MainPID", "0"))
        if pid > 0:
            for name in ("status", "wchan", "limits"):
                try:
                    (directory / ("process-" + name + ".txt")).write_text(
                        Path(f"/proc/{pid}/{name}").read_text()[:65536])
                except OSError:
                    pass
        return props
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        (directory / "diagnostics-error.txt").write_text(str(error))
        return {}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--memory-gib", type=float, default=8,
                        help="hard resident cgroup memory limit, including descendants (default: 8)")
    parser.add_argument("--runtime", type=float, default=600,
                        help="hard total service lifetime in seconds (default: 600)")
    parser.add_argument("--startup-timeout", type=float, default=90)
    parser.add_argument("--hang-timeout", type=float, default=30)
    parser.add_argument("--stall-timeout", type=float, default=120,
                        help="stop a running, loaded game with no frames or shader work for N seconds; 0 disables")
    parser.add_argument("--output", required=True, type=Path,
                        help="new run directory for socket, screenshots, logs and manifest")
    parser.add_argument("--screenshot-interval", type=float, default=0,
                        help="keep the latest GUI screenshot every N seconds; 0 disables")
    parser.add_argument("--debugger", action="store_true",
                          help="launch under gdb to record all thread stacks on a crash")
    parser.add_argument("--background", action="store_true",
                        help="keep Eden below other apps without desktop input grabs (X11/Xwayland)")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- /absolute/path/to/eden [args]")
    args = parser.parse_args(argv)
    for name in ("memory_gib", "runtime", "startup_timeout", "hang_timeout", "stall_timeout", "screenshot_interval"):
        if not math.isfinite(getattr(args, name)):
            parser.error(name + " must be finite")
    if not 0.25 <= args.memory_gib <= 64:
        parser.error("memory-gib must be in [0.25, 64]")
    if not 5 <= args.runtime <= 86400 or args.startup_timeout <= 0 or args.hang_timeout < 2:
        parser.error("runtime must be 5..86400, startup-timeout > 0, hang-timeout >= 2 seconds")
    if args.screenshot_interval < 0:
        parser.error("screenshot-interval must be >= 0")
    if args.stall_timeout < 0:
        parser.error("stall-timeout must be >= 0")
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("supply the Eden executable after --")
    if sys.platform != "linux":
        parser.error("this bounded launcher requires Linux systemd and cgroup memory accounting")
    try:
        os.environ.setdefault("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
        os.environ.setdefault("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + os.environ["XDG_RUNTIME_DIR"] + "/bus")
        check = bounded(["systemctl", "--user", "show-environment"])
        if check.returncode:
            raise RuntimeError("systemd user manager unavailable: " + check.stderr.strip())
        for line in check.stdout.splitlines():
            name, separator, value = line.partition("=")
            if separator and name in ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY"):
                os.environ.setdefault(name, value)
        # X11 window capture includes native Vulkan/OpenGL children and works under Xwayland.
        # An explicit QT_QPA_PLATFORM continues to select another backend.
        os.environ.setdefault("QT_QPA_PLATFORM", "xcb")
        directory = args.output.expanduser().resolve()
        directory.mkdir(parents=True, exist_ok=False, mode=0o700)
        os.chmod(directory, 0o700)
        socket_path = directory / "control.sock"
        if len(os.fsencode(socket_path)) > 100:
            raise ValueError("run directory path is too long for a Unix socket; use a shorter path")
        unit = "eden-control-" + uuid.uuid4().hex[:12] + ".service"
        memory_bytes = int(args.memory_gib * 1024 ** 3)
        invocation = ["systemd-run", "--user", "--quiet", "--wait", "--pipe", "--unit=" + unit,
                      "--property=MemoryMax=" + str(memory_bytes), "--property=MemorySwapMax=0",
                      "--property=MemoryAccounting=yes", "--property=OOMPolicy=stop",
                      "--property=RuntimeMaxSec=" + str(args.runtime), "--property=TimeoutStopSec=5",
                      "--property=KillMode=control-group", "--property=TasksMax=1024",
                      "--property=LimitCORE=0",
                      "--property=WorkingDirectory=" + str(Path.cwd())]
        for name in ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR",
                     "DBUS_SESSION_BUS_ADDRESS", "LD_LIBRARY_PATH", "QT_PLUGIN_PATH",
                     "QT_QPA_PLATFORM", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME",
                     "VK_ICD_FILENAMES", "VK_DRIVER_FILES", "EDEN_LOG_FILTER"):
            if name in os.environ:
                invocation.append("--setenv=" + name + "=" + os.environ[name])
        child_command = [*command, "--control-socket", str(socket_path),
                         "--control-log", str(directory / "control.jsonl"), "--control-verbose"]
        if args.background:
            child_command.append("--control-background")
        if args.debugger:
            debugger = shutil.which("gdb")
            if not debugger:
                raise RuntimeError("--debugger requires gdb")
            child_command = [debugger, "--batch", "--return-child-result", "-ex", "set pagination off",
                             "-ex", "set startup-with-shell off", "-ex", "set debuginfod enabled off",
                             "-ex", "set print thread-events off", "-ex", "run", "-ex", "thread apply all bt",
                             "--args", *child_command]
        invocation += ["--", *child_command]
        state = {"unit": unit, "socket": str(socket_path), "command": command,
                 "memory_max_bytes": memory_bytes, "memory_swap_max_bytes": 0,
                 "runtime_seconds": args.runtime, "startup_timeout": args.startup_timeout,
                 "hang_timeout": args.hang_timeout, "started_at": time.time(), "state": "starting"}
        state["debugger"] = args.debugger
        save(directory / "run.json", state)
        log = logging.getLogger(unit)
        log.setLevel(logging.INFO)
        handler = logging.handlers.RotatingFileHandler(directory / "console.log", maxBytes=8*1024*1024,
                                                       backupCount=3, encoding="utf-8")
        handler.setFormatter(logging.Formatter("%(message)s"))
        log.addHandler(handler)
        process = subprocess.Popen(invocation, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL)
        def drain():
            while True:
                chunk = process.stdout.read1(65536)
                if not chunk:
                    break
                log.info(chunk.decode("utf-8", errors="replace").rstrip("\n"))
        reader = threading.Thread(target=drain, daemon=True)
        reader.start()
        interrupted = threading.Event()
        for sig in (signal.SIGINT, signal.SIGTERM):
            signal.signal(sig, lambda *_: interrupted.set())
        started = time.monotonic()
        last_ok = None
        stalled_since = None
        last_screenshot = started
        last_resources = started
        reason = "exited"
        client = Client(socket_path, timeout=min(2, args.hang_timeout / 2))
        print(json.dumps(state), flush=True)
        try:
            while process.poll() is None:
                now = time.monotonic()
                if interrupted.is_set():
                    reason = "interrupted"
                    break
                if now - started >= args.runtime:
                    reason = "runtime_limit"
                    break
                if last_ok is None and now - started > args.startup_timeout:
                    reason = "startup_timeout"
                    break
                if last_ok is not None and now - last_ok > args.hang_timeout:
                    reason = "unresponsive"
                    break
                try:
                    reply = client.request("ping")
                    if reply.get("ok"):
                        last_ok = time.monotonic()
                        if state["state"] == "starting":
                            actual = diagnostics(unit, directory)
                            if (actual.get("MemoryMax") != str(memory_bytes) or
                                    actual.get("MemorySwapMax") != "0" or
                                    duration_seconds(actual.get("RuntimeMaxUSec")) is None or
                                    abs(duration_seconds(actual.get("RuntimeMaxUSec")) - args.runtime) > 0.001):
                                reason = "resource_limits_unverified"
                                break
                            state["verified_limits"] = actual
                            state["last_resources"] = actual
                            peak = actual.get("MemoryPeak", "")
                            state["memory_peak_bytes_observed"] = int(peak) if peak.isdecimal() else 0
                            state.update(state="ready", ready_at=time.time())
                            save(directory / "run.json", state)
                        if now - last_resources >= 5:
                            actual = diagnostics(unit, directory)
                            state["last_resources"] = actual
                            peak = actual.get("MemoryPeak", "")
                            if peak.isdecimal():
                                state["memory_peak_bytes_observed"] = max(
                                    state.get("memory_peak_bytes_observed", 0), int(peak))
                            state["resources_sampled_at"] = time.time()
                            save(directory / "run.json", state)
                            last_resources = now
                        if args.stall_timeout:
                            status = client.request("status")
                            details = status.get("result", {}) if status.get("ok") else {}
                            stalled = (details.get("running") and details.get("loading_complete") and
                                       not details.get("modal") and not details.get("shutting_down") and
                                       details.get("shaders_building") == 0 and details.get("game_fps") == 0)
                            stalled_since = (stalled_since or now) if stalled else None
                            state["last_status"] = details
                            if stalled_since is not None and now - stalled_since >= args.stall_timeout:
                                reason = "game_stalled"
                                break
                        if args.screenshot_interval and now - last_screenshot >= args.screenshot_interval:
                            capture = client.request("ui.screenshot", {"target": "active", "path": str(directory / "latest.png")})
                            state["last_screenshot_response"] = capture
                            if capture.get("ok"):
                                last_screenshot = now
                except (OSError, ValueError, ControlError):
                    pass
                interrupted.wait(0.5)
        finally:
            state["unit_before_stop"] = diagnostics(unit, directory)
            if reason == "exited":
                result = state["unit_before_stop"].get("Result")
                if result == "oom-kill":
                    reason = "memory_limit"
                elif result == "timeout":
                    # systemd also uses timeout when an external stop reaches
                    # TimeoutStopSec (for example, a shutdown confirmation dialog).
                    reason = ("runtime_limit" if time.monotonic() - started >= args.runtime
                              else "stop_timeout")
                elif result in ("signal", "core-dump"):
                    reason = "crashed"
            if process.poll() is None:
                stop_unit(unit)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            reader.join(timeout=3)
            handler.close()
            state.update(state="stopped", reason=reason, exit_code=process.returncode,
                         ended_at=time.time(), elapsed_seconds=time.monotonic()-started)
            save(directory / "run.json", state)
            print(json.dumps(state), flush=True)
        return 0 if reason == "exited" and process.returncode == 0 else 1
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(json.dumps({"ok": False, "error": str(error)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
