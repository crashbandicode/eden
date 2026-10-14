#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independently validate the GIFs written by gif_writer_test.

Three decoders must agree for every recording:
  * a strict block parser plus a from-scratch LZW decoder (this file),
  * Pillow's GIF plugin,
  * the raw source frames dumped by the encoder test (exact, or within one dithered RGB332 step).

Usage: validate_gifs.py <output directory of gif_writer_test>
"""

import json
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # Fail loudly: a skipped validation would hide encoder bugs.
    sys.exit("validate_gifs.py needs Pillow (python3 -m pip install pillow)")

# Largest per-channel distance between a source value and its dithered RGB332 reconstruction:
# 0.75 palette steps (0.5 rounding plus a quarter step of dither) plus palette rounding.
TOLERANCE = (28, 28, 64)

failures = []


def fail(case, message):
    failures.append(f"{case}: {message}")


def palette_level(index, levels):
    return (index * 255 + levels // 2) // levels


def expected_palette():
    """RGB332: index bits RRRGGGBB, each channel scaled to 0..255 and rounded."""
    return bytes(
        value
        for i in range(256)
        for value in (
            palette_level((i >> 5) & 7, 7),
            palette_level((i >> 2) & 7, 7),
            palette_level(i & 3, 3),
        )
    )


EXPECTED_PALETTE = expected_palette()


class GifError(Exception):
    pass


def read_sub_blocks(data, pos):
    """Returns (payload, next position); sub-blocks end with a zero-length block."""
    chunks = []
    while True:
        if pos >= len(data):
            raise GifError("truncated data sub-blocks")
        size = data[pos]
        pos += 1
        if size == 0:
            return b"".join(chunks), pos
        if pos + size > len(data):
            raise GifError("sub-block runs past end of file")
        chunks.append(data[pos:pos + size])
        pos += size


def parse_gif(data):
    """Strict structural parse; only accepts the layout the writer promises."""
    if data[:6] != b"GIF89a":
        raise GifError(f"bad signature {data[:6]!r}")
    width = int.from_bytes(data[6:8], "little")
    height = int.from_bytes(data[8:10], "little")
    packed = data[10]
    if not packed & 0x80:
        raise GifError("no global colour table")
    if packed & 0x07 != 7:
        raise GifError("global colour table is not 256 entries")
    if packed & 0x08:
        raise GifError("global colour table is flagged as sorted")
    palette = data[13:13 + 768]
    pos = 13 + 768

    gif = {"width": width, "height": height, "palette": palette, "loops": [], "frames": []}
    pending_gce = None
    while True:
        if pos >= len(data):
            raise GifError("missing trailer")
        block = data[pos]
        pos += 1
        if block == 0x3B:
            if pos != len(data):
                raise GifError(f"{len(data) - pos} bytes after trailer")
            if pending_gce is not None:
                raise GifError("graphic control extension without an image")
            return gif
        if block == 0x21:
            label = data[pos]
            pos += 1
            if label == 0xFF:  # Application extension.
                if data[pos] != 11:
                    raise GifError("bad application extension header size")
                app = data[pos + 1:pos + 12]
                payload, pos = read_sub_blocks(data, pos + 12)
                if app == b"NETSCAPE2.0":
                    if gif["frames"]:
                        raise GifError("loop extension after the first frame")
                    if len(payload) != 3 or payload[0] != 1:
                        raise GifError(f"bad NETSCAPE payload {payload!r}")
                    gif["loops"].append(int.from_bytes(payload[1:3], "little"))
            elif label == 0xF9:  # Graphic control extension.
                if pending_gce is not None:
                    raise GifError("two graphic control extensions in a row")
                if data[pos] != 4:
                    raise GifError("bad graphic control block size")
                flags = data[pos + 1]
                pending_gce = {
                    "disposal": (flags >> 2) & 7,
                    "user_input": bool(flags & 2),
                    "transparent": bool(flags & 1),
                    "delay_cs": int.from_bytes(data[pos + 2:pos + 4], "little"),
                    "transparent_index": data[pos + 4],
                }
                if data[pos + 5] != 0:
                    raise GifError("graphic control extension not terminated")
                pos += 6
            else:
                raise GifError(f"unexpected extension 0x{label:02X}")
        elif block == 0x2C:
            if pending_gce is None:
                raise GifError("image without graphic control extension")
            left = int.from_bytes(data[pos:pos + 2], "little")
            top = int.from_bytes(data[pos + 2:pos + 4], "little")
            w = int.from_bytes(data[pos + 4:pos + 6], "little")
            h = int.from_bytes(data[pos + 6:pos + 8], "little")
            flags = data[pos + 8]
            pos += 9
            if flags & 0x80:
                raise GifError("unexpected local colour table")
            min_code_size = data[pos]
            pos += 1
            lzw, pos = read_sub_blocks(data, pos)
            gif["frames"].append({
                **pending_gce, "left": left, "top": top, "width": w, "height": h,
                "interlaced": bool(flags & 0x40), "min_code_size": min_code_size, "lzw": lzw,
            })
            pending_gce = None
        else:
            raise GifError(f"unexpected block 0x{block:02X} at offset {pos - 1}")


def lzw_decode(data, min_code_size, pixel_count):
    """Textbook GIF LZW decoder. Returns (indices, stats); raises GifError on any violation."""
    clear = 1 << min_code_size
    end = clear + 1
    width = min_code_size + 1
    table = [bytes([i]) for i in range(clear)] + [b"", b""]
    out = bytearray()
    previous = None
    clears = 0
    max_width = width
    code_widths = []

    bit_buffer = 0
    bit_count = 0
    index = 0
    reached_end = False
    while True:
        while bit_count < width:
            if index >= len(data):
                raise GifError("LZW data ended before the end code")
            bit_buffer |= data[index] << bit_count
            bit_count += 8
            index += 1
        code = bit_buffer & ((1 << width) - 1)
        bit_buffer >>= width
        bit_count -= width
        code_widths.append(width)

        if len(code_widths) == 1 and code != clear:
            raise GifError(f"stream starts with code {code}, not a clear code")
        if code == clear:
            clears += 1
            table = table[:clear + 2]
            width = min_code_size + 1
            previous = None
            continue
        if code == end:
            reached_end = True
            break
        if previous is None:
            if code >= clear:
                raise GifError(f"first code after clear is {code}")
            entry = table[code]
        else:
            if code < len(table):
                entry = table[code]
            elif code == len(table):
                entry = previous + previous[:1]
            else:
                raise GifError(f"code {code} beyond dictionary size {len(table)}")
            if len(table) < 4096:
                table.append(previous + entry[:1])
        out += entry
        previous = entry
        if len(table) == (1 << width) and width < 12:
            width += 1
        max_width = max(max_width, width)

    if not reached_end:
        raise GifError("missing end code")
    if len(out) != pixel_count:
        raise GifError(f"decoded {len(out)} pixels, expected {pixel_count}")
    # Remaining bits must be zero padding within the final byte.
    if index != len(data) or bit_buffer != 0:
        raise GifError("data after the end code")
    stats = {
        "clears": clears,
        "max_width": max_width,
        "last_data_width": code_widths[-2],
        "end_width": code_widths[-1],
    }
    return bytes(out), stats


def indices_to_rgb(indices, palette):
    rgb = bytearray(3 * len(indices))
    for channel in range(3):
        rgb[channel::3] = indices.translate(bytes(palette[i * 3 + channel] for i in range(256)))
    return bytes(rgb)


def within_tolerance(decoded, source):
    worst = [0, 0, 0]
    for i, (a, b) in enumerate(zip(decoded, source)):
        d = abs(a - b)
        if d > worst[i % 3]:
            worst[i % 3] = d
    return all(w <= limit for w, limit in zip(worst, TOLERANCE)), worst


def validate_case(root, case, meta):
    name = case["name"]
    w, h = case["width"], case["height"]
    delays = case["delays_cs"]
    path = root / case["file"]
    data = path.read_bytes()

    try:
        gif = parse_gif(data)
    except GifError as e:
        fail(name, f"structure: {e}")
        return
    except IndexError:
        fail(name, "structure: truncated file")
        return

    if (gif["width"], gif["height"]) != (w, h):
        fail(name, f"logical screen {gif['width']}x{gif['height']}, expected {w}x{h}")
    if gif["palette"] != EXPECTED_PALETTE:
        fail(name, "global palette is not the RGB332 palette")
    if gif["loops"] != [0]:
        fail(name, f"loop extensions {gif['loops']}, expected exactly one infinite loop")
    if len(gif["frames"]) != len(delays):
        fail(name, f"{len(gif['frames'])} frames, expected {len(delays)}")
        return
    if case["max_bytes"] and len(data) > case["max_bytes"]:
        fail(name, f"file is {len(data)} bytes, expected at most {case['max_bytes']}")

    decoded_frames = []
    for i, (frame, delay) in enumerate(zip(gif["frames"], delays)):
        tag = f"frame {i}"
        if frame["delay_cs"] != delay:
            fail(name, f"{tag}: delay {frame['delay_cs']} cs, expected {delay}")
        if frame["disposal"] != 1:
            fail(name, f"{tag}: disposal {frame['disposal']}, expected 1")
        if frame["transparent"]:
            fail(name, f"{tag}: transparency flag set")
        if (frame["left"], frame["top"], frame["width"], frame["height"]) != (0, 0, w, h):
            fail(name, f"{tag}: image rectangle is not the full frame")
        if frame["interlaced"]:
            fail(name, f"{tag}: interlaced")
        if frame["min_code_size"] != 8:
            fail(name, f"{tag}: minimum code size {frame['min_code_size']}")
        try:
            indices, stats = lzw_decode(frame["lzw"], frame["min_code_size"], w * h)
        except GifError as e:
            fail(name, f"{tag}: LZW: {e}")
            decoded_frames.append(None)
            continue
        meta["frame_stats"].append(stats)
        decoded_frames.append(indices_to_rgb(indices, gif["palette"]))

    # Compare against the source frames the encoder was given.
    for i, rgb in enumerate(decoded_frames):
        if rgb is None:
            continue
        source = (root / case["frames"][i]).read_bytes()
        if len(source) != w * h * 3:
            fail(name, f"frame {i}: source dump has {len(source)} bytes")
            continue
        if case["exact"]:
            if rgb != source:
                bad = next(j for j in range(len(rgb)) if rgb[j] != source[j]) // 3
                fail(name, f"frame {i}: pixel ({bad % w}, {bad // w}) differs from the source")
        else:
            ok, worst = within_tolerance(rgb, source)
            if not ok:
                fail(name, f"frame {i}: per-channel error {worst} exceeds {TOLERANCE}")

    # Pillow must read the same thing: frame count, timing, loop, colours, orientation.
    try:
        with Image.open(path) as im:
            if im.size != (w, h):
                fail(name, f"Pillow size {im.size}")
            if getattr(im, "n_frames", 1) != len(delays):
                fail(name, f"Pillow n_frames {getattr(im, 'n_frames', 1)}, expected {len(delays)}")
            if im.info.get("loop") != 0:
                fail(name, f"Pillow loop {im.info.get('loop')!r}, expected 0")
            if im.info.get("version") != b"GIF89a":
                fail(name, f"Pillow version {im.info.get('version')!r}")
            if im.getpalette()[:768] != list(EXPECTED_PALETTE):
                fail(name, "Pillow palette differs from RGB332")
            for i, delay in enumerate(delays):
                im.seek(i)
                if im.info.get("duration") != delay * 10:
                    fail(name, f"frame {i}: Pillow duration {im.info.get('duration')} ms, "
                               f"expected {delay * 10}")
                if "transparency" in im.info:
                    fail(name, f"frame {i}: Pillow reports transparency")
                if getattr(im, "disposal_method", 1) != 1:
                    fail(name, f"frame {i}: Pillow disposal {im.disposal_method}")
                pillow_rgb = im.convert("RGB").tobytes()
                if decoded_frames[i] is not None and pillow_rgb != decoded_frames[i]:
                    fail(name, f"frame {i}: Pillow pixels differ from the independent decoder")
                if name == "anim_exact":
                    check_orientation(name, im.convert("RGB"))
    except Exception as e:  # Pillow raises many exception types on corrupt data.
        fail(name, f"Pillow could not read the GIF: {type(e).__name__}: {e}")


def check_orientation(name, image):
    """The first synthetic pattern has red/green/blue/white markers in the four corners."""
    w, h = image.size
    for label, xy, color in (
        ("top-left", (0, 0), (255, 0, 0)),
        ("top-right", (w - 1, 0), (0, 255, 0)),
        ("bottom-left", (0, h - 1), (0, 0, 255)),
        ("bottom-right", (w - 1, h - 1), (255, 255, 255)),
    ):
        if image.getpixel(xy) != color:
            fail(name, f"{label} marker is {image.getpixel(xy)}, expected {color}")


def check_coverage(meta):
    """Make sure the corpus really exercised the LZW corner cases it is meant to."""
    stats = meta["frame_stats"]
    if not stats:
        fail("coverage", "no frames were decoded")
        return
    if not any(s["clears"] >= 3 for s in stats):
        fail("coverage", "no frame contained a mid-stream dictionary reset")
    if not any(s["max_width"] == 12 for s in stats):
        fail("coverage", "no frame reached 12-bit codes")
    widths = {(s["last_data_width"], s["end_width"]) for s in stats}
    for w in (9, 10, 11):
        if (w, w + 1) not in widths:
            fail("coverage", f"no frame widened the end code from {w} to {w + 1} bits")
        if (w, w) not in widths:
            fail("coverage", f"no frame kept the end code at {w} bits near that boundary")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1])
    manifest = json.loads((root / "manifest.json").read_text())
    cases = manifest["cases"]
    if not cases:
        sys.exit("manifest contains no cases")

    meta = {"frame_stats": []}
    for case in cases:
        validate_case(root, case, meta)
    check_coverage(meta)

    # Nothing but the GIFs the manifest lists may have been left in the output directory.
    listed = {Path(c["file"]).name for c in cases}
    present = {p.name for p in (root / "gifs").iterdir()}
    if present != listed:
        fail("directory", f"unexpected files in gifs/: {sorted(present ^ listed)[:5]}")

    frames = sum(len(c["delays_cs"]) for c in cases)
    print(f"validated {len(cases)} GIFs, {frames} frames, {len(failures)} problems")
    for line in failures[:50]:
        print("FAIL", line)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
