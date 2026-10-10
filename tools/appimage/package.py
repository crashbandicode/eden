#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Package a trusted local Eden build for a matching Linux distribution.

Uses native cc/mksquashfs and an official type-2 runtime; executes no shell scripts.
The payload retains debug symbols. GPU drivers and glibc come from the host.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def run(args, **kwargs):
    return subprocess.run(args, check=True, timeout=kwargs.pop("timeout", 120), **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--dependency-root", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new directory")
    parser.add_argument("--name", default="Eden-CLI-Control-20261005-x86_64.AppImage",
                        help="AppImage filename (without directories)")
    args = parser.parse_args()
    if Path(args.name).name != args.name or not args.name.endswith(".AppImage"):
        parser.error("--name must be an .AppImage filename without directories")
    source = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    app = output / "Eden.AppDir"
    lib = app / "usr/lib"
    binary = app / "usr/bin/eden"
    lib.mkdir(parents=True)
    binary.parent.mkdir(parents=True)
    shutil.copy2(args.binary, binary)
    env = dict(os.environ, LD_LIBRARY_PATH=str(args.dependency_root / "usr/lib/x86_64-linux-gnu"))
    plugin_root = args.dependency_root / "usr/lib/x86_64-linux-gnu/qt6/plugins"
    plugin_categories = ("imageformats", "xcbglintegrations", "platforminputcontexts", "styles",
                         "tls", "wayland-decoration-client", "wayland-graphics-integration-client",
                         "wayland-shell-integration")
    seeds = [args.binary]
    for plugin in sorted(plugin_root.rglob("*.so")):
        relative = plugin.relative_to(plugin_root)
        if relative.parts[0] not in plugin_categories and not (
                relative.parts[0] == "platforms" and plugin.name in
                ("libqxcb.so", "libqoffscreen.so", "libqminimal.so", "libqwayland.so")):
            continue
        destination = app / "usr/plugins" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(plugin, destination)
        seeds.append(plugin)

    # Keep the loader, glibc, graphics driver dispatch and Vulkan loader on the host.
    system = {"libc.so.6", "libm.so.6", "libmvec.so.1", "libdl.so.2", "libpthread.so.0",
              "librt.so.1", "libresolv.so.2", "libutil.so.1", "ld-linux-x86-64.so.2",
              "libGL.so.1", "libGLX.so.0", "libEGL.so.1", "libOpenGL.so.0",
              "libGLdispatch.so.0", "libvulkan.so.1", "libgbm.so.1", "libdrm.so.2"}
    libraries = {}
    host_libraries = set()
    visited = set()
    while seeds:
        elf = seeds.pop()
        if elf.resolve() in visited:
            continue
        visited.add(elf.resolve())
        result = run(["ldd", str(elf)], env=env, capture_output=True, text=True)
        if "not found" in result.stdout:
            raise RuntimeError(f"Unresolved dependency of {elf}:\n{result.stdout}")
        for line in result.stdout.splitlines():
            match = re.match(r"\s*(\S+) => (/\S+) \(", line)
            if not match:
                continue
            name, origin = match.groups()
            if name in system or name.startswith("libnvidia"):
                host_libraries.add(name)
                continue
            if name not in libraries:
                origin = Path(origin).resolve()
                libraries[name] = str(origin)
                shutil.copy2(origin, lib / name)
                seeds.append(origin)

    run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", str(source / "tools/appimage/AppRun.c"),
         "-o", str(app / "AppRun")])
    (binary.parent / "qt.conf").write_text("[Paths]\nPrefix=..\nPlugins=plugins\n")
    desktop = source / "dist/dev.eden_emu.eden.desktop"
    shutil.copy2(desktop, app / desktop.name)
    shutil.copy2(source / "dist/dev.eden_emu.eden.svg", app / "dev.eden_emu.eden.svg")
    shutil.copy2(source / "dist/qt_themes/default/icons/256x256/eden.png", app / ".DirIcon")
    share = app / "usr/share"
    (share / "applications").mkdir(parents=True)
    shutil.copy2(desktop, share / "applications" / desktop.name)
    (share / "metainfo").mkdir()
    shutil.copy2(source / "dist/dev.eden_emu.eden.metainfo.xml", share / "metainfo")
    (share / "licenses/eden").mkdir(parents=True)
    shutil.copy2(source / "LICENSE.txt", share / "licenses/eden/LICENSE.txt")
    shutil.copytree(source / "LICENSES", share / "licenses/eden/LICENSES")
    # Include package copyright notices, without copying unrelated manuals.
    for copyright_file in (args.dependency_root / "usr/share/doc").glob("*/copyright"):
        dest = share / "licenses/distribution" / copyright_file.parent.name / "copyright"
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(copyright_file, dest)
    manifest = {"format": 1, "base_commit": run(["git", "-C", str(source), "rev-parse", "HEAD"],
                capture_output=True, text=True).stdout.strip(), "debug_symbols": "retained",
                "runtime_sha256": hashlib.file_digest(args.runtime.open("rb"), "sha256").hexdigest(),
                "libraries": libraries, "host_libraries": sorted(host_libraries),
                "target": "Ubuntu 26.04 x86_64 (glibc 2.43)"}
    (output / "package-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    shutil.copy2(output / "package-manifest.json", share / "package-manifest.json")
    payload = output / "payload.squashfs"
    run(["mksquashfs", str(app), str(payload), "-noappend", "-all-root", "-comp", "zstd",
         "-Xcompression-level", "10", "-processors", "2", "-mem", "256M", "-no-progress"],
        timeout=900)
    image = output / args.name
    with image.open("wb") as dest:
        for part in (args.runtime, payload):
            with part.open("rb") as src:
                shutil.copyfileobj(src, dest, 1024 * 1024)
    image.chmod(0o755)
    print(json.dumps({"appimage": str(image), "bytes": image.stat().st_size,
                      "bundled_libraries": len(libraries)}))


if __name__ == "__main__":
    main()
