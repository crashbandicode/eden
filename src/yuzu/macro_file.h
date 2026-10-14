// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <vector>

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

/**
 * Input macro document model and its on-disk codec ("eden-input-macro", version 1).
 *
 * Depends on QtCore and the standard library only. Every time in a document is a frame number
 * of the emulated display (not wall-clock time). Events are sparse, full-state snapshots ordered
 * by frame: a snapshot holds until the next event, and the last one holds until duration_frames.
 *
 * JSON layout (all keys are mandatory, unknown keys are rejected, numbers must be JSON numbers,
 * integers must be integral, strings must be strings; there is no implicit coercion):
 *
 *   {
 *     "format": "eden-input-macro",
 *     "version": 1,
 *     "name": "...",                 trimmed, 1..128 code points
 *     "game_id": "" | "0100ABCD...", empty or exactly 16 hex digits
 *     "player": 0..9,
 *     "duration_frames": 1..216000,
 *     "bindings": { "keyboard": "...", "gamepad": "..." },   each <= 256 code points, may be ""
 *     "parent":   { "name": "...", "marker": "..." },        both "" or both set (branches)
 *     "markers": [ { "name": "...", "frame": 0..duration_frames }, ... ],   <= 1024
 *     "events": [                                           1..216000, strictly ascending frame
 *       { "frame": 0..duration_frames-1,                    first event is at frame 0
 *         "buttons": 0..4194303,                            22-bit mask, bit i = ButtonNames()[i]
 *         "sticks": [lx, ly, rx, ry],                       4 numbers in [-1, 1]
 *         "triggers": [left, right],                        2 numbers in [0, 1]
 *         "motion": [ {                                     exactly 2 entries (device 0, device 1)
 *             "delta_us": 1..1000000,
 *             "gyro":  [x, y, z],                           3 numbers in [-100, 100]
 *             "accel": [x, y, z] }, {...} ] },              3 numbers in [-100, 100]
 *       ... ]
 *   }
 *
 * Text rules: names (macro, marker, parent) are non-empty, already trimmed (no leading/trailing
 * whitespace), at most 128 Unicode code points; all strings must be well-formed UTF-16 without
 * control characters. Marker names are unique and case-sensitive. A macro is never its own
 * parent. Shortcut strings are only length/shape checked here; their meaning belongs to the GUI.
 */
namespace EdenMacro {

constexpr int ButtonCount = 22;
constexpr quint64 MaxFrames = 216000;
constexpr int MaxEvents = 216000;
constexpr qint64 MaxFileBytes = qint64{64} * 1024 * 1024;

// Additional limits enforced by Validate().
constexpr int MaxMarkers = 1024;
constexpr int MaxNameChars = 128;
constexpr int MaxShortcutChars = 256;
constexpr int GameIdChars = 16;
constexpr int MaxPlayer = 9;
constexpr qint64 MinMotionDeltaUs = 1;
constexpr qint64 MaxMotionDeltaUs = 1000000;
constexpr int FormatVersion = 1;
constexpr quint32 ButtonMask = (quint32{1} << ButtonCount) - 1;

struct Motion {
    qint64 delta_us{10000};
    std::array<float, 3> gyro{};
    std::array<float, 3> accel{0, 0, -1};

    bool operator==(const Motion&) const = default;
};

struct Sample {
    quint32 buttons{};
    std::array<float, 4> sticks{};
    std::array<float, 2> triggers{};
    std::array<Motion, 2> motion{};

    bool operator==(const Sample&) const = default;
};

struct Event {
    quint64 frame{};
    Sample sample;
};

struct Marker {
    QString name;
    quint64 frame{};
};

struct Document {
    QString name;
    QString game_id;
    int player{};
    quint64 duration_frames{};
    std::vector<Event> events;
    std::vector<Marker> markers;
    QString keyboard_shortcut;
    QString gamepad_shortcut;
    QString parent_name;
    QString parent_marker;
};

/// The value of the JSON "format" key.
QString FormatName();

/// The 22 button names in bit order (identical to control_input.cpp): a, b, x, y, lstick,
/// rstick, l, r, zl, zr, plus, minus, left, up, right, down, sl_left, sr_left, home, capture,
/// sl_right, sr_right.
QStringList ButtonNames();

/// Strict check of every rule listed above. On failure returns false and sets error (location
/// and reason); on success clears error. A recording draft without events (see MakePrefix at
/// marker frame 0) is not valid: Save() refuses it until it has recorded frames.
bool Validate(const Document& document, QString& error);

/// Reads a macro file. Files larger than MaxFileBytes are rejected without being parsed. The
/// result is only assigned when the whole file parses and validates; out is untouched on failure.
bool Load(const QString& path, Document& out, QString& error);

/// Validates, then writes the file atomically (QSaveFile). An existing file at path is refused
/// unless overwrite is true; a symbolic link or non-regular file at path is always refused (a
/// link is never written through). Never creates directories. The serialized form must not
/// exceed MaxFileBytes. The no-overwrite check is repeated right before the commit but, as with
/// any rename-based commit, is not atomic against another process creating the file in that
/// instant.
bool Save(const QString& path, const Document& document, QString& error, bool overwrite = false);

/// Serializes without validating (non-finite numbers become null and fail FromJson).
QJsonObject ToJson(const Document& document);

/// Strict inverse of ToJson (see the layout above); validates the result. out is untouched on
/// failure.
bool FromJson(const QJsonObject& object, Document& out, QString& error);

/// Creates a self-contained branch draft from the start of source up to the named marker.
///
/// The source is validated first. The result owns copies of every event with frame < the marker
/// frame and every marker with frame <= it, has duration_frames equal to the marker frame, the
/// source's game_id/player, name new_name (same rules as any name, and different from the
/// source's name), parent_name = source.name, parent_marker = marker_name, and no playback
/// bindings. The source is never modified. A marker at the very end clones the whole prefix.
/// A marker at frame 0 yields an empty draft (no events, duration 0): fine for the recorder to
/// append to, but it fails Validate()/Save() until recorded frames exist. The recorder appends
/// events with frame >= duration_frames and then extends duration_frames. out is untouched on
/// failure and may alias source.
bool MakePrefix(const Document& source, const QString& marker_name, const QString& new_name,
                Document& out, QString& error);

/// The state in effect at frame: the last event with event.frame <= frame (so frames at or past
/// the end hold the final state). Returns a static neutral Sample if there is no such event
/// (empty document). The reference stays valid as long as the document is not modified.
const Sample& SampleAt(const Document& document, quint64 frame);

} // namespace EdenMacro
