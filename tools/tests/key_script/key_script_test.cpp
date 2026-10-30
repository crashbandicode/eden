// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Direct tests of Nx2KeyScript::Load, Advance and Stop. Expected event fields and
// callback sequences are written out from the startup contract. They are not
// produced by re-implementing the parser.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "yuzu_cmd/nx2_key_script.h"

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* expression, const char* file, int line, const std::string& detail) {
    ++g_checks;
    if (ok) {
        return;
    }
    ++g_failed;
    std::fprintf(stderr, "FAIL %s:%d: %s %s\n", file, line, expression, detail.c_str());
}

#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__, {})
#define CHECK_MSG(expression, detail) \
    Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__, (detail))

// Documented bounds, written as literals so a drift in the header constants fails.
constexpr std::size_t kByteLimit = 1048576;
constexpr std::size_t kEventLimit = 4096;
constexpr std::size_t kTokenLimit = 64;
constexpr int kScancodeLast = 511;

struct Action {
    bool release = false;
    int scancode = 0;

    bool operator==(const Action& other) const {
        return release == other.release && scancode == other.scancode;
    }
};

struct Recorder {
    std::vector<Action> log;

    Nx2KeyScript::Callbacks callbacks() {
        Nx2KeyScript::Callbacks out;
        out.press = [this](int scancode) { log.push_back(Action{false, scancode}); };
        out.release = [this](int scancode) { log.push_back(Action{true, scancode}); };
        return out;
    }
};

std::string FormatLog(const std::vector<Action>& log) {
    std::string text;
    for (const Action& action : log) {
        text.push_back(action.release ? '-' : '+');
        text += std::to_string(action.scancode);
        text.push_back(' ');
    }
    return text;
}

bool LoadChecked(std::string_view spec, Nx2KeyScript::Session& session, std::string& error) {
    try {
        return Nx2KeyScript::Load(spec, session, error);
    } catch (const std::exception& ex) {
        error = std::string("uncaught exception: ") + ex.what();
        CHECK_MSG(false, error);
        return false;
    } catch (...) {
        error = "uncaught exception";
        CHECK_MSG(false, error);
        return false;
    }
}

void AdvanceChecked(Nx2KeyScript::Session& session, double elapsed, std::uint64_t present,
                    const Nx2KeyScript::Callbacks& callbacks) {
    try {
        Nx2KeyScript::Advance(session, elapsed, present, callbacks);
    } catch (const std::exception& ex) {
        CHECK_MSG(false, std::string("uncaught exception: ") + ex.what());
    } catch (...) {
        CHECK_MSG(false, "uncaught exception");
    }
}

void StopChecked(Nx2KeyScript::Session& session, const Nx2KeyScript::Callbacks& callbacks) {
    try {
        Nx2KeyScript::Stop(session, callbacks);
    } catch (const std::exception& ex) {
        CHECK_MSG(false, std::string("uncaught exception: ") + ex.what());
    } catch (...) {
        CHECK_MSG(false, "uncaught exception");
    }
}

bool Near(double value, double expected) {
    return std::isfinite(value) && std::abs(value - expected) <= 1e-9;
}

void ExpectReject(const std::string& spec, const char* needle) {
    Nx2KeyScript::Session session;
    std::string error;
    CHECK(LoadChecked("0:4", session, error));
    const bool accepted = LoadChecked(spec, session, error);
    CHECK_MSG(!accepted, error);
    CHECK_MSG(error.find(needle) != std::string::npos, error + " (missing " + needle + ")");
    CHECK(session.events.size() == 1);
    CHECK(session.events[0].scancode == 4);
    CHECK(session.events[0].clock == Nx2KeyScript::Clock::Seconds);
}

void ExpectLog(const std::vector<Action>& log, const std::vector<Action>& expected) {
    CHECK_MSG(log == expected, FormatLog(log) + " != " + FormatLog(expected));
}

void TestParseKnownScript() {
    constexpr std::string_view kNotes =
        "50:4,50.5:-4,55:79,55.4:-79,57:4,57.5:-4";
    Nx2KeyScript::Session session;
    std::string error;
    CHECK(LoadChecked(kNotes, session, error));
    CHECK_MSG(error.empty(), error);
    CHECK(session.events.size() == 6);
    const double seconds[] = {50, 50.5, 55, 55.4, 57, 57.5};
    const int codes[] = {4, 4, 79, 79, 4, 4};
    const bool release[] = {false, true, false, true, false, true};
    if (session.events.size() != 6) {
        return;
    }
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(session.events[i].clock == Nx2KeyScript::Clock::Seconds);
        CHECK(Near(session.events[i].seconds, seconds[i]));
        CHECK(session.events[i].present == 0);
        CHECK(session.events[i].scancode == codes[i]);
        CHECK(session.events[i].release == release[i]);
    }
}

void TestFrameZeroIsNotTimeZero() {
    Nx2KeyScript::Session present_session;
    Nx2KeyScript::Session time_session;
    std::string error;
    CHECK(LoadChecked("f0:79", present_session, error));
    CHECK(present_session.events.size() == 1);
    CHECK(present_session.events[0].clock == Nx2KeyScript::Clock::Present);
    CHECK(present_session.events[0].present == 0);
    CHECK(present_session.events[0].scancode == 79);
    CHECK(!present_session.events[0].release);

    CHECK(LoadChecked("0:79", time_session, error));
    CHECK(time_session.events.size() == 1);
    CHECK(time_session.events[0].clock == Nx2KeyScript::Clock::Seconds);
    CHECK(time_session.events[0].seconds == 0.0);
    CHECK(time_session.events[0].scancode == 79);

    CHECK(LoadChecked("f12:-3", present_session, error));
    CHECK(present_session.events.size() == 1);
    CHECK(present_session.events[0].clock == Nx2KeyScript::Clock::Present);
    CHECK(present_session.events[0].present == 12);
    CHECK(present_session.events[0].release);
    CHECK(present_session.events[0].scancode == 3);

    CHECK(LoadChecked("+1.5:7", time_session, error));
    CHECK(time_session.events[0].clock == Nx2KeyScript::Clock::Seconds);
    CHECK(time_session.events[0].seconds == 1.5);
    CHECK(time_session.events[0].scancode == 7);

    CHECK(LoadChecked("1e1:8", time_session, error));
    CHECK(time_session.events[0].seconds == 10.0);
    CHECK(time_session.events[0].scancode == 8);

    CHECK(LoadChecked("  50 : 4 , f0 : 79 ", time_session, error));
    CHECK(time_session.events.size() == 2);
    CHECK(time_session.events[0].clock == Nx2KeyScript::Clock::Seconds);
    CHECK(time_session.events[0].seconds == 50.0);
    CHECK(time_session.events[0].scancode == 4);
    CHECK(time_session.events[1].clock == Nx2KeyScript::Clock::Present);
    CHECK(time_session.events[1].present == 0);
    CHECK(time_session.events[1].scancode == 79);

    CHECK(LoadChecked("0:4,", time_session, error));
    CHECK(time_session.events.size() == 1);
    CHECK(LoadChecked("0:4,\r\n1:5", time_session, error));
    CHECK(time_session.events.size() == 2);
    CHECK(time_session.events[1].seconds == 1.0);
    CHECK(time_session.events[1].scancode == 5);

    CHECK(LoadChecked("", time_session, error));
    CHECK(time_session.events.empty());
    CHECK(LoadChecked("   \n\t", time_session, error));
    CHECK(time_session.events.empty());

    CHECK(LoadChecked("0:0", time_session, error));
    CHECK(time_session.events[0].scancode == 0);
    CHECK(!time_session.events[0].release);
    CHECK(LoadChecked("0:-0", time_session, error));
    CHECK(time_session.events[0].scancode == 0);
    CHECK(time_session.events[0].release);
    CHECK(LoadChecked("0:511", time_session, error));
    CHECK(time_session.events[0].scancode == kScancodeLast);
    CHECK(LoadChecked("0:-511", time_session, error));
    CHECK(time_session.events[0].scancode == kScancodeLast);
    CHECK(time_session.events[0].release);
    CHECK(LoadChecked("f18446744073709551615:1", present_session, error));
    CHECK(present_session.events[0].present == ~std::uint64_t{0});
}

void TestRejections() {
    ExpectReject("nope", "missing ':'");
    ExpectReject("1", "missing ':'");
    ExpectReject("1:2:3", "malformed entry");
    ExpectReject(":4", "missing time");
    ExpectReject("1:", "missing scancode");
    ExpectReject("-1:4", "negative time");
    ExpectReject("-0:4", "negative time");
    ExpectReject("-0.0:4", "negative time");
    ExpectReject("+-0:4", "negative time");
    ExpectReject("inf:4", "non-finite time");
    ExpectReject("nan:4", "non-finite time");
    ExpectReject("1e9999:4", "out-of-range time");
    ExpectReject("1.5abc:4", "trailing characters in time");
    ExpectReject("abc:4", "malformed time");
    ExpectReject("f:4", "malformed present index");
    ExpectReject("f-1:4", "negative present index");
    ExpectReject("f1abc:4", "trailing characters in present index");
    ExpectReject("fabc:4", "malformed present index");
    ExpectReject("f18446744073709551616:4", "present index is out of range");
    ExpectReject("1:4xyz", "trailing characters in scancode");
    ExpectReject("1:4.0", "trailing characters in scancode");
    ExpectReject("1:xyz", "malformed scancode");
    ExpectReject("1:-", "malformed scancode");
    ExpectReject("1:512", "scancode is outside 0..511");
    ExpectReject("1:-512", "scancode is outside 0..511");
    ExpectReject("1:99999999999999999999", "scancode is out of range");
    ExpectReject("0:4,,1:5", "empty entry");
    ExpectReject("@", "file path is empty");
    ExpectReject("@   ", "file path is empty");
    ExpectReject("@/no/such/eden-nx2-key-script", "file not found: /no/such/eden-nx2-key-script");
    ExpectReject("@/", "cannot read file: /");

    ExpectReject(std::string(kTokenLimit + 1, '1') + ":4", "oversized time token");
    ExpectReject("0:" + std::string(kTokenLimit + 1, '9'), "oversized scancode token");
    ExpectReject("f" + std::string(kTokenLimit + 1, '1') + ":4", "oversized present token");
    ExpectReject(std::string(kByteLimit + 1, 'x'), "byte limit of 1048576 exceeded");

    std::string too_many;
    too_many.reserve((kEventLimit + 1) * 4);
    for (std::size_t i = 0; i < kEventLimit + 1; ++i) {
        if (i != 0) {
            too_many.push_back(',');
        }
        too_many += "0:1";
    }
    ExpectReject(too_many, "event limit of 4096 exceeded");
}

void TestEventLimitBoundary() {
    std::string text;
    text.reserve(kEventLimit * 4);
    for (std::size_t i = 0; i < kEventLimit; ++i) {
        if (i != 0) {
            text.push_back(',');
        }
        text += "0:1";
    }
    Nx2KeyScript::Session session;
    std::string error;
    CHECK(LoadChecked(text, session, error));
    CHECK_MSG(error.empty(), error);
    CHECK(session.events.size() == kEventLimit);
    CHECK(session.events.front().scancode == 1);
    CHECK(session.events.back().scancode == 1);

    std::string at_limit(kByteLimit, ' ');
    at_limit.replace(0, 3, "0:4");
    CHECK(at_limit.size() == kByteLimit);
    CHECK(LoadChecked(at_limit, session, error));
    CHECK(session.events.size() == 1);
    CHECK(session.events[0].scancode == 4);
}

void TestFailedLoadLeavesSession() {
    Nx2KeyScript::Session session;
    std::string error;
    CHECK(LoadChecked("0:8,5:-8", session, error));
    Recorder recorder;
    const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
    AdvanceChecked(session, 0.0, 0, callbacks);
    CHECK(session.next == 1);
    CHECK(session.held.size() == 1);
    CHECK(session.held[0] == 8);
    CHECK(!session.closed);
    const std::size_t next = session.next;
    const std::vector<int> held = session.held;
    CHECK(!LoadChecked("1:512,0:1", session, error));
    CHECK(error.find("scancode is outside 0..511") != std::string::npos);
    CHECK(session.next == next);
    CHECK(session.held == held);
    CHECK(!session.closed);
    CHECK(session.events.size() == 2);
    CHECK(session.events[0].scancode == 8);
    CHECK(session.events[1].release);
    ExpectLog(recorder.log, {Action{false, 8}});
}

void TestFileScript() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "nx2-key-script-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "script.txt";
    {
        std::ofstream output(file, std::ios::binary);
        output << "f0:4,1.5:-4";
    }
    Nx2KeyScript::Session session;
    std::string error;
    const std::string spec = "@" + file.string();
    CHECK(LoadChecked(spec, session, error));
    CHECK_MSG(error.empty(), error);
    CHECK(session.events.size() == 2);
    CHECK(session.events[0].clock == Nx2KeyScript::Clock::Present);
    CHECK(session.events[0].present == 0);
    CHECK(session.events[0].scancode == 4);
    CHECK(!session.events[0].release);
    CHECK(session.events[1].clock == Nx2KeyScript::Clock::Seconds);
    CHECK(Near(session.events[1].seconds, 1.5));
    CHECK(session.events[1].release);
    CHECK(session.events[1].scancode == 4);

    const std::filesystem::path huge = dir / "huge.txt";
    {
        std::ofstream output(huge, std::ios::binary);
        output << std::string(kByteLimit + 1, 'x');
    }
    Nx2KeyScript::Session preserved = session;
    CHECK(!LoadChecked("@" + huge.string(), session, error));
    CHECK(error.find("byte limit of 1048576 exceeded") != std::string::npos);
    CHECK(session.events.size() == preserved.events.size());
    CHECK(session.events[0].clock == Nx2KeyScript::Clock::Present);

    const std::filesystem::path missing = dir / "missing.txt";
    CHECK(!LoadChecked("@" + missing.string(), session, error));
    CHECK(error.find("file not found: " + missing.string()) != std::string::npos);
    CHECK(session.events[0].scancode == 4);

    // A mistaken non-script file must not leak its contents into an error/log.
    {
        std::ofstream output(file, std::ios::binary);
        output << "PRIVATE_TEST_SENTINEL\nSECOND_LINE";
    }
    CHECK(!LoadChecked(spec, session, error));
    CHECK(error == "entry 1: missing ':'");
    CHECK(error.find("PRIVATE_TEST_SENTINEL") == std::string::npos);
    CHECK(error.find('\n') == std::string::npos);

    std::filesystem::remove_all(dir, ec);
}

void TestListedOrderAndClocks() {
    // Later due entry waits behind an earlier entry that is not due.
    {
        Nx2KeyScript::Session session;
        std::string error;
        CHECK(LoadChecked("2:4,f0:5", session, error));
        Recorder recorder;
        const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
        AdvanceChecked(session, 0.0, 99, callbacks);
        ExpectLog(recorder.log, {});
        CHECK(session.next == 0);
        CHECK(!session.closed);
        AdvanceChecked(session, 2.0, 99, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}, Action{false, 5}});
        CHECK(!session.closed);
        CHECK(session.held.size() == 2);
        AdvanceChecked(session, 10.0, 99, callbacks);
        CHECK(recorder.log.size() == 2);
        StopChecked(session, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}, Action{false, 5}, Action{true, 4}, Action{true, 5}});
        CHECK(recorder.log.size() == 4);
    }
    {
        Nx2KeyScript::Session session;
        std::string error;
        CHECK(LoadChecked("f2:4,0:5", session, error));
        Recorder recorder;
        const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
        AdvanceChecked(session, 100.0, 0, callbacks);
        ExpectLog(recorder.log, {});
        AdvanceChecked(session, 100.0, 2, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}, Action{false, 5}});
        StopChecked(session, callbacks);
    }
    // f0 is due at present 0 even when elapsed time is negative. Time 0 is not.
    {
        Nx2KeyScript::Session session;
        std::string error;
        CHECK(LoadChecked("f0:4", session, error));
        Recorder recorder;
        const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
        AdvanceChecked(session, -1.0, 0, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}});
        CHECK(!session.closed);
        StopChecked(session, callbacks);
    }
    {
        Nx2KeyScript::Session session;
        std::string error;
        CHECK(LoadChecked("0:4", session, error));
        Recorder recorder;
        const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
        AdvanceChecked(session, -1.0, 100, callbacks);
        ExpectLog(recorder.log, {});
        CHECK(!session.closed);
        AdvanceChecked(session, 0.0, 100, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}});
        StopChecked(session, callbacks);
    }
    // Not sorted by time: 5s stays ahead of 1s.
    {
        Nx2KeyScript::Session session;
        std::string error;
        CHECK(LoadChecked("5:4,1:8", session, error));
        Recorder recorder;
        const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
        AdvanceChecked(session, 4.0, 0, callbacks);
        ExpectLog(recorder.log, {});
        AdvanceChecked(session, 10.0, 0, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}, Action{false, 8}});
        StopChecked(session, callbacks);
    }
    // Boundary: seconds and presents use <=.
    {
        Nx2KeyScript::Session session;
        std::string error;
        CHECK(LoadChecked("1:4", session, error));
        Recorder recorder;
        const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
        AdvanceChecked(session, 0.5, 100, callbacks);
        ExpectLog(recorder.log, {});
        AdvanceChecked(session, 1.0, 0, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}});
        StopChecked(session, callbacks);
    }
    {
        Nx2KeyScript::Session session;
        std::string error;
        CHECK(LoadChecked("f1:4", session, error));
        Recorder recorder;
        const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
        AdvanceChecked(session, 1000.0, 0, callbacks);
        ExpectLog(recorder.log, {});
        AdvanceChecked(session, 0.0, 1, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}});
        StopChecked(session, callbacks);
    }
}

void TestOrderedElapsedScript() {
    Nx2KeyScript::Session session;
    std::string error;
    CHECK(LoadChecked("50:4,50.5:-4,55:79,55.5:-79,57:4,57.5:-4", session, error));
    Recorder recorder;
    const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
    AdvanceChecked(session, 49.0, 0, callbacks);
    ExpectLog(recorder.log, {});
    AdvanceChecked(session, 50.0, 3, callbacks);
    ExpectLog(recorder.log, {Action{false, 4}});
    CHECK(session.held.size() == 1);
    CHECK(session.held[0] == 4);
    AdvanceChecked(session, 50.5, 3, callbacks);
    ExpectLog(recorder.log, {Action{false, 4}, Action{true, 4}});
    CHECK(session.held.empty());
    CHECK(!session.closed);
    AdvanceChecked(session, 55.0, 3, callbacks);
    ExpectLog(recorder.log, {Action{false, 4}, Action{true, 4}, Action{false, 79}});
    AdvanceChecked(session, 55.5, 3, callbacks);
    ExpectLog(recorder.log,
              {Action{false, 4}, Action{true, 4}, Action{false, 79}, Action{true, 79}});
    AdvanceChecked(session, 57.0, 3, callbacks);
    ExpectLog(recorder.log, {Action{false, 4}, Action{true, 4}, Action{false, 79}, Action{true, 79},
                             Action{false, 4}});
    AdvanceChecked(session, 57.5, 3, callbacks);
    ExpectLog(recorder.log, {Action{false, 4}, Action{true, 4}, Action{false, 79}, Action{true, 79},
                             Action{false, 4}, Action{true, 4}});
    CHECK(session.closed);
    CHECK(session.held.empty());
    const std::size_t after = recorder.log.size();
    AdvanceChecked(session, 100.0, 100, callbacks);
    CHECK(recorder.log.size() == after);
}

void TestHeldKeyCleanup() {
    // Unpaired presses survive list exhaustion and subsequent polling. Explicit
    // shutdown releases them exactly once, in press order.
    // Scancode 22 stands for a user key this script never names.
    {
        Nx2KeyScript::Session session;
        std::string error;
        CHECK(LoadChecked("0:8,0:4", session, error));
        Recorder recorder;
        const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
        AdvanceChecked(session, 0.0, 0, callbacks);
        ExpectLog(recorder.log, {Action{false, 8}, Action{false, 4}});
        CHECK(!session.closed);
        CHECK(session.next == session.events.size());
        AdvanceChecked(session, 10.0, 100, callbacks);
        ExpectLog(recorder.log, {Action{false, 8}, Action{false, 4}});
        StopChecked(session, callbacks);
        ExpectLog(recorder.log, {Action{false, 8}, Action{false, 4}, Action{true, 8}, Action{true, 4}});
        CHECK(session.closed);
        CHECK(session.held.empty());
        StopChecked(session, callbacks);
        AdvanceChecked(session, 20.0, 200, callbacks);
        CHECK(recorder.log.size() == 4);
        for (const Action& action : recorder.log) {
            CHECK(action.scancode != 22);
        }
    }
    // A matching release is one callback, not a second cleanup release.
    {
        Nx2KeyScript::Session session;
        std::string error;
        CHECK(LoadChecked("0:8,1:-8", session, error));
        Recorder recorder;
        const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
        AdvanceChecked(session, 0.0, 0, callbacks);
        ExpectLog(recorder.log, {Action{false, 8}});
        CHECK(session.held.size() == 1);
        AdvanceChecked(session, 1.0, 0, callbacks);
        ExpectLog(recorder.log, {Action{false, 8}, Action{true, 8}});
        CHECK(session.closed);
        int releases = 0;
        for (const Action& action : recorder.log) {
            if (action.release && action.scancode == 8) {
                ++releases;
            }
            CHECK(action.scancode != 22);
        }
        CHECK(releases == 1);
    }
    // Shutdown while a press is still held releases that scancode and drops the rest.
    {
        Nx2KeyScript::Session session;
        std::string error;
        CHECK(LoadChecked("0:4,5:-4,6:8", session, error));
        Recorder recorder;
        const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
        AdvanceChecked(session, 0.0, 0, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}});
        CHECK(!session.closed);
        StopChecked(session, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}, Action{true, 4}});
        CHECK(session.closed);
        CHECK(session.held.empty());
        AdvanceChecked(session, 10.0, 10, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}, Action{true, 4}});
        StopChecked(session, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}, Action{true, 4}});
        for (const Action& action : recorder.log) {
            CHECK(action.scancode != 22);
        }
    }
    // A negative entry still releases the scancode it names, even if the script
    // did not press it. Cleanup does not add releases for any other scancode.
    {
        Nx2KeyScript::Session session;
        std::string error;
        CHECK(LoadChecked("0:-4", session, error));
        Recorder recorder;
        const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
        AdvanceChecked(session, 0.0, 0, callbacks);
        ExpectLog(recorder.log, {Action{true, 4}});
        CHECK(session.closed);
    }
    // Repeated press is held once, so shutdown releases it once.
    {
        Nx2KeyScript::Session session;
        std::string error;
        CHECK(LoadChecked("0:4,0:4", session, error));
        Recorder recorder;
        const Nx2KeyScript::Callbacks callbacks = recorder.callbacks();
        AdvanceChecked(session, 0.0, 0, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}, Action{false, 4}});
        StopChecked(session, callbacks);
        ExpectLog(recorder.log, {Action{false, 4}, Action{false, 4}, Action{true, 4}});
    }
}

} // namespace

int main() {
    try {
        TestParseKnownScript();
        TestFrameZeroIsNotTimeZero();
        TestRejections();
        TestEventLimitBoundary();
        TestFailedLoadLeavesSession();
        TestFileScript();
        TestListedOrderAndClocks();
        TestOrderedElapsedScript();
        TestHeldKeyCleanup();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "uncaught exception: %s\n", ex.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "uncaught exception\n");
        return 1;
    }
    if (g_failed != 0) {
        std::fprintf(stderr, "%d checks, %d failed\n", g_checks, g_failed);
        return 1;
    }
    std::printf("%d checks, 0 failed\n", g_checks);
    return 0;
}
