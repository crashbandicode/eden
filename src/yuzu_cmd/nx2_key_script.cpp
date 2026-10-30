// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/nx2_key_script.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace Nx2KeyScript {
namespace {

std::string EntryPrefix(std::size_t entry) {
    return "entry " + std::to_string(entry) + ": ";
}

bool IsAsciiSpace(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && IsAsciiSpace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsAsciiSpace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

bool ParseSeconds(std::string_view token, std::size_t entry, double& seconds, std::string& error) {
    const std::string prefix = EntryPrefix(entry);
    if (token.empty()) {
        error = prefix + "missing time";
        return false;
    }
    if (token.size() > MaxNumericToken) {
        error = prefix + "oversized time token";
        return false;
    }
    if (token.front() == '-') {
        error = prefix + "negative time";
        return false;
    }
    std::string_view number = token;
    if (number.front() == '+') {
        number.remove_prefix(1);
        if (number.empty()) {
            error = prefix + "malformed time";
            return false;
        }
    }
    double parsed = 0;
    const char* const begin = number.data();
    const char* const end = begin + number.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);
    if (result.ec == std::errc::result_out_of_range) {
        error = prefix + "non-finite or out-of-range time";
        return false;
    }
    if (result.ec == std::errc{} && result.ptr != end) {
        error = prefix + "trailing characters in time";
        return false;
    }
    if (result.ec != std::errc{} || result.ptr != end) {
        error = prefix + "malformed time";
        return false;
    }
    if (!std::isfinite(parsed)) {
        error = prefix + "non-finite time";
        return false;
    }
    if (std::signbit(parsed)) {
        error = prefix + "negative time";
        return false;
    }
    seconds = parsed;
    return true;
}

bool ParsePresent(std::string_view token, std::size_t entry, std::uint64_t& present,
                  std::string& error) {
    const std::string prefix = EntryPrefix(entry);
    // token includes the leading 'f'.
    if (token.size() <= 1) {
        error = prefix + "malformed present index";
        return false;
    }
    const std::string_view number = token.substr(1);
    if (number.size() > MaxNumericToken) {
        error = prefix + "oversized present token";
        return false;
    }
    if (number.front() == '-') {
        error = prefix + "negative present index";
        return false;
    }
    std::uint64_t parsed = 0;
    const char* const begin = number.data();
    const char* const end = begin + number.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);
    if (result.ec == std::errc::result_out_of_range) {
        error = prefix + "present index is out of range";
        return false;
    }
    if (result.ec == std::errc{} && result.ptr != end) {
        error = prefix + "trailing characters in present index";
        return false;
    }
    if (result.ec != std::errc{} || result.ptr != end) {
        error = prefix + "malformed present index";
        return false;
    }
    present = parsed;
    return true;
}

bool ParseScancode(std::string_view token, std::size_t entry, int& scancode, bool& release,
                   std::string& error) {
    const std::string prefix = EntryPrefix(entry);
    if (token.empty()) {
        error = prefix + "missing scancode";
        return false;
    }
    if (token.size() > MaxNumericToken) {
        error = prefix + "oversized scancode token";
        return false;
    }
    release = false;
    std::string_view number = token;
    if (number.front() == '-') {
        release = true;
        number.remove_prefix(1);
        if (number.empty()) {
            error = prefix + "malformed scancode";
            return false;
        }
    }
    int magnitude = 0;
    const char* const begin = number.data();
    const char* const end = begin + number.size();
    const std::from_chars_result result = std::from_chars(begin, end, magnitude);
    if (result.ec == std::errc::result_out_of_range) {
        error = prefix + "scancode is out of range";
        return false;
    }
    if (result.ec == std::errc{} && result.ptr != end) {
        error = prefix + "trailing characters in scancode";
        return false;
    }
    if (result.ec != std::errc{} || result.ptr != end || magnitude < 0) {
        error = prefix + "malformed scancode";
        return false;
    }
    if (magnitude >= ScancodeCount) {
        error = prefix + "scancode is outside 0..511";
        return false;
    }
    scancode = magnitude;
    return true;
}

bool ParseEntry(std::string_view item, std::size_t entry, std::vector<Event>& events,
                std::string& error) {
    const std::size_t colon = item.find(':');
    if (colon == std::string_view::npos) {
        error = EntryPrefix(entry) + "missing ':'";
        return false;
    }
    if (item.find(':', colon + 1) != std::string_view::npos) {
        error = EntryPrefix(entry) + "malformed entry";
        return false;
    }
    const std::string_view when = Trim(item.substr(0, colon));
    const std::string_view code = Trim(item.substr(colon + 1));
    Event event{};
    if (!when.empty() && when.front() == 'f') {
        event.clock = Clock::Present;
        if (!ParsePresent(when, entry, event.present, error)) {
            return false;
        }
    } else {
        event.clock = Clock::Seconds;
        if (!ParseSeconds(when, entry, event.seconds, error)) {
            return false;
        }
    }
    if (!ParseScancode(code, entry, event.scancode, event.release, error)) {
        return false;
    }
    events.push_back(event);
    return true;
}

bool ParseList(std::string_view text, std::vector<Event>& events, std::string& error) {
    text = Trim(text);
    if (text.empty()) {
        return true;
    }
    std::size_t entry = 1;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const bool last = comma == std::string_view::npos;
        const std::size_t stop = last ? text.size() : comma;
        const std::string_view item = Trim(text.substr(start, stop - start));
        if (item.empty()) {
            if (last) {
                return true;
            }
            error = EntryPrefix(entry) + "empty entry";
            return false;
        }
        if (events.size() == MaxEvents) {
            error = "event limit of " + std::to_string(MaxEvents) + " exceeded";
            return false;
        }
        if (!ParseEntry(item, entry, events, error)) {
            return false;
        }
        if (last) {
            return true;
        }
        start = comma + 1;
        ++entry;
    }
    return true;
}

bool ReadScriptFile(std::string_view path_text, std::string& text, std::string& error) {
    const std::string path(path_text);
    const std::filesystem::path file(path);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        error = "file not found: " + path;
        return false;
    }
    ec.clear();
    if (!std::filesystem::is_regular_file(file, ec) || ec) {
        error = "cannot read file: " + path;
        return false;
    }
    ec.clear();
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        error = "cannot read file: " + path;
        return false;
    }
    if (size > static_cast<std::uintmax_t>(MaxScriptBytes)) {
        error = "byte limit of " + std::to_string(MaxScriptBytes) + " exceeded";
        return false;
    }
    std::ifstream input(file, std::ios::in | std::ios::binary);
    if (!input) {
        error = "cannot read file: " + path;
        return false;
    }
    const auto bytes = static_cast<std::size_t>(size);
    text.assign(bytes, '\0');
    if (bytes > 0) {
        input.read(text.data(), static_cast<std::streamsize>(bytes));
        if (!input) {
            text.clear();
            error = "cannot read file: " + path;
            return false;
        }
    }
    return true;
}

void ReleaseHeld(Session& session, const Callbacks& callbacks) {
    if (!callbacks.release || session.held.empty()) {
        return;
    }
    const std::vector<int> held = session.held;
    session.held.clear();
    for (const int scancode : held) {
        callbacks.release(scancode);
    }
}

void Dispatch(Session& session, const Event& event, const Callbacks& callbacks) {
    if (event.release) {
        const auto it = std::find(session.held.begin(), session.held.end(), event.scancode);
        if (it != session.held.end()) {
            session.held.erase(it);
        }
        if (callbacks.release) {
            callbacks.release(event.scancode);
        }
        return;
    }
    if (std::find(session.held.begin(), session.held.end(), event.scancode) == session.held.end()) {
        session.held.push_back(event.scancode);
    }
    if (callbacks.press) {
        callbacks.press(event.scancode);
    }
}

} // namespace

bool Load(std::string_view specification, Session& session, std::string& error) {
    error.clear();
    if (specification.size() > MaxScriptBytes) {
        error = "byte limit of " + std::to_string(MaxScriptBytes) + " exceeded";
        return false;
    }
    const std::string_view trimmed = Trim(specification);
    std::string file_text;
    std::string_view body = trimmed;
    if (!trimmed.empty() && trimmed.front() == '@') {
        const std::string_view path = Trim(trimmed.substr(1));
        if (path.empty()) {
            error = "file path is empty";
            return false;
        }
        if (!ReadScriptFile(path, file_text, error)) {
            return false;
        }
        body = file_text;
    }
    Session parsed;
    if (!ParseList(body, parsed.events, error)) {
        return false;
    }
    session = std::move(parsed);
    return true;
}

void Advance(Session& session, double elapsed_seconds, std::uint64_t present_count,
             const Callbacks& callbacks) {
    if (session.closed) {
        return;
    }
    while (session.next < session.events.size()) {
        const Event& event = session.events[session.next];
        const bool due = event.clock == Clock::Present ? event.present <= present_count
                                                        : event.seconds <= elapsed_seconds;
        if (!due) {
            return;
        }
        Dispatch(session, event, callbacks);
        ++session.next;
        if (session.closed) {
            return;
        }
    }
    // Preserve a final unpaired press until explicit Stop. Releasing it in this
    // call could make it invisible to the guest's independently scheduled HID poll.
    if (session.held.empty()) {
        session.closed = true;
    }
}

void Stop(Session& session, const Callbacks& callbacks) {
    if (session.closed) {
        return;
    }
    if (!session.held.empty() && !callbacks.release) {
        return;
    }
    ReleaseHeld(session, callbacks);
    session.closed = true;
}

} // namespace Nx2KeyScript
