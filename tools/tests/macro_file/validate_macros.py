# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Independent validator for the macro files written by macro_file_test.

It re-reads every file with Python's json module (strict: no NaN/Infinity, no duplicate keys),
checks the "eden-input-macro" version 1 layout, types and limits exactly as specified in
src/yuzu/macro_file.h without using any C++ code, and compares the content with manifest.json.

    python3 -I validate_macros.py <directory written by macro_file_test>
"""

import json
import math
import os
import struct
import sys
import unicodedata

BUTTON_BITS = 22
MAX_FRAMES = 216000
MAX_EVENTS = 216000
MAX_MARKERS = 1024
errors = []


def fail(where, message):
    errors.append(f"{where}: {message}")


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def reject_constant(name):
    raise ValueError(f"non-finite JSON constant {name}")


def reject_duplicates(pairs):
    keys = [key for key, _ in pairs]
    if len(keys) != len(set(keys)):
        raise ValueError("duplicate key")
    return dict(pairs)


def is_int(value):
    return type(value) is int


def is_number(value):
    return type(value) in (int, float) and math.isfinite(value)


def valid_text(text, limit):
    if not isinstance(text, str) or len(text) > limit:
        return False
    for ch in text:
        if unicodedata.category(ch) in ("Cc", "Cs"):
            return False
    return True


def valid_name(text):
    return valid_text(text, 128) and text != "" and text == text.strip()


def keys_exactly(where, obj, keys):
    if not isinstance(obj, dict) or set(obj) != set(keys):
        fail(where, f"keys {sorted(obj) if isinstance(obj, dict) else type(obj)} != {sorted(keys)}")
        return False
    return True


def floats(where, value, count, low, high):
    if not isinstance(value, list) or len(value) != count:
        fail(where, f"expected {count} numbers")
        return False
    for item in value:
        if not is_number(item) or not low <= item <= high:
            fail(where, f"bad number {item!r}")
            return False
    return True


def check_document(where, doc):
    if not keys_exactly(where, doc, ["format", "version", "name", "game_id", "player",
                                     "duration_frames", "bindings", "parent", "markers", "events"]):
        return False
    if doc["format"] != "eden-input-macro" or not is_int(doc["version"]) or doc["version"] != 1:
        fail(where, "format/version")
    if not valid_name(doc["name"]):
        fail(where, "name")
    game_id = doc["game_id"]
    if not isinstance(game_id, str) or not (game_id == "" or (
            len(game_id) == 16 and all(c in "0123456789abcdefABCDEF" for c in game_id))):
        fail(where, "game_id")
    if not is_int(doc["player"]) or not 0 <= doc["player"] <= 9:
        fail(where, "player")
    duration = doc["duration_frames"]
    if not is_int(duration) or not 1 <= duration <= MAX_FRAMES:
        fail(where, "duration_frames")
        return False
    if keys_exactly(where + ".bindings", doc["bindings"], ["keyboard", "gamepad"]):
        for key, text in doc["bindings"].items():
            if not valid_text(text, 256):
                fail(where + ".bindings", key)
    if keys_exactly(where + ".parent", doc["parent"], ["name", "marker"]):
        parent, marker = doc["parent"]["name"], doc["parent"]["marker"]
        if (parent == "") != (marker == ""):
            fail(where + ".parent", "name and marker must be set together")
        elif parent != "" and not (valid_name(parent) and valid_name(marker)):
            fail(where + ".parent", "invalid names")
        if parent != "" and parent == doc["name"]:
            fail(where + ".parent", "own parent")

    markers = doc["markers"]
    if not isinstance(markers, list) or len(markers) > MAX_MARKERS:
        fail(where, "markers")
        markers = []
    seen = set()
    for i, marker in enumerate(markers):
        here = f"{where}.markers[{i}]"
        if keys_exactly(here, marker, ["name", "frame"]):
            if not valid_name(marker["name"]) or marker["name"] in seen:
                fail(here, "name")
            seen.add(marker["name"])
            if not is_int(marker["frame"]) or not 0 <= marker["frame"] <= duration:
                fail(here, "frame")

    events = doc["events"]
    if not isinstance(events, list) or not 1 <= len(events) <= MAX_EVENTS:
        fail(where, "events")
        return False
    previous = -1
    for i, event in enumerate(events):
        here = f"{where}.events[{i}]"
        if not keys_exactly(here, event, ["frame", "buttons", "sticks", "triggers", "motion"]):
            continue
        frame = event["frame"]
        if not is_int(frame) or frame <= previous or frame >= duration or (i == 0 and frame != 0):
            fail(here, f"frame {frame!r}")
        else:
            previous = frame
        if not is_int(event["buttons"]) or not 0 <= event["buttons"] < (1 << BUTTON_BITS):
            fail(here, "buttons")
        floats(here + ".sticks", event["sticks"], 4, -1, 1)
        floats(here + ".triggers", event["triggers"], 2, 0, 1)
        motion = event["motion"]
        if not isinstance(motion, list) or len(motion) != 2:
            fail(here, "motion count")
            continue
        for j, item in enumerate(motion):
            there = f"{here}.motion[{j}]"
            if keys_exactly(there, item, ["delta_us", "gyro", "accel"]):
                if not is_int(item["delta_us"]) or not 1 <= item["delta_us"] <= 1000000:
                    fail(there, "delta_us")
                floats(there + ".gyro", item["gyro"], 3, -100, 100)
                floats(there + ".accel", item["accel"], 3, -100, 100)
    return True


def flatten(event):
    ints = [event["frame"], event["buttons"], event["motion"][0]["delta_us"],
            event["motion"][1]["delta_us"]]
    values = list(event["sticks"]) + list(event["triggers"])
    for item in event["motion"]:
        values += list(item["gyro"]) + list(item["accel"])
    return ints, values


def compare_manifest(where, doc, entry):
    pairs = [("name", doc["name"]), ("game_id", doc["game_id"]), ("player", doc["player"]),
             ("duration_frames", doc["duration_frames"]), ("keyboard", doc["bindings"]["keyboard"]),
             ("gamepad", doc["bindings"]["gamepad"]), ("parent_name", doc["parent"]["name"]),
             ("parent_marker", doc["parent"]["marker"])]
    for key, actual in pairs:
        if entry[key] != actual:
            fail(where, f"{key}: file has {actual!r}, expected {entry[key]!r}")
    if [[m["name"], m["frame"]] for m in doc["markers"]] != entry["markers"]:
        fail(where, "markers differ")
    if len(doc["events"]) != len(entry["event_ints"]):
        fail(where, "event count differs")
        return
    for i, event in enumerate(doc["events"]):
        ints, values = flatten(event)
        if ints != entry["event_ints"][i]:
            fail(where, f"events[{i}] integers {ints} != {entry['event_ints'][i]}")
        expected = entry["event_floats"][i]
        # The file may store a shorter decimal; it must still denote exactly the same float32.
        if len(values) != len(expected) or any(f32(a) != f32(b) for a, b in zip(values, expected)):
            fail(where, f"events[{i}] floats differ")


def main():
    directory = sys.argv[1]
    with open(os.path.join(directory, "manifest.json"), "rb") as handle:
        manifest = json.loads(handle.read().decode("utf-8"))
    names = set(os.listdir(directory)) - {"manifest.json"}
    listed = {entry["file"] for entry in manifest["files"]}
    if names != listed:
        fail("directory", f"unexpected content {sorted(names ^ listed)}")
    for entry in manifest["files"]:
        where = entry["file"]
        with open(os.path.join(directory, where), "rb") as handle:
            raw = handle.read()
        if raw.startswith(b"\xef\xbb\xbf") or not raw.endswith(b"\n") or raw.count(b"\n") != 1:
            fail(where, "expected one line of UTF-8 JSON ending in a newline, without BOM")
        try:
            doc = json.loads(raw.decode("utf-8"), parse_constant=reject_constant,
                             object_pairs_hook=reject_duplicates)
        except ValueError as exc:
            fail(where, f"invalid JSON: {exc}")
            continue
        if check_document(where, doc):
            compare_manifest(where, doc, entry)
    if errors:
        print("\n".join(errors[:50]))
        print(f"{len(errors)} problem(s)")
        return 1
    print(f"validated {len(manifest['files'])} macro files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
