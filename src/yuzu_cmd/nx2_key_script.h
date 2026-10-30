// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Nx2KeyScript {

// EDEN_NX2_KEY_SCRIPT is read once, before emulator/game initialization.
// There is no status or cancel API. Live changes to the variable are ignored.

inline constexpr std::size_t MaxScriptBytes = 1048576;
inline constexpr std::size_t MaxEvents = 4096;
inline constexpr std::size_t MaxNumericToken = 64;
inline constexpr int ScancodeCount = 512;

enum class Clock : std::uint8_t {
    Seconds, // host seconds from the SDL key clock; finite and >= 0
    Present, // Core present count, including 0 (f0)
};

struct Event {
    Clock clock = Clock::Seconds;
    double seconds = 0;
    std::uint64_t present = 0;
    int scancode = 0; // 0 .. ScancodeCount-1
    bool release = false;
};

// Progress for one SDL frontend instance. Not process-static.
struct Session {
    std::vector<Event> events;
    std::size_t next = 0;
    std::vector<int> held; // scancodes this script pressed and has not released, in press order
    bool closed = false;
};

struct Callbacks {
    std::function<void(int scancode)> press;
    std::function<void(int scancode)> release;
};

// Parse an inline list or "@<file>". On failure, session is unchanged and error is set.
// Empty text is an empty script. Does not throw on malformed numbers.
bool Load(std::string_view specification, Session& session, std::string& error);

// Run every leading entry whose own clock is due, in listed order. A later entry
// does not pass an earlier one that is not due. A negative entry releases the
// scancode it names. Exhausting the list preserves any unpaired presses until
// Stop; otherwise the guest could miss a press immediately followed by cleanup.
void Advance(Session& session, double elapsed_seconds, std::uint64_t present_count,
             const Callbacks& callbacks);

// Release scancodes the script is still holding and ignore any entries not yet run.
// Does not release scancodes the script did not press. Idempotent once closed.
void Stop(Session& session, const Callbacks& callbacks);

} // namespace Nx2KeyScript
