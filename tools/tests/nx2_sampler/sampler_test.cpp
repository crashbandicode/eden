// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Original probe: real Session entry points; named, independently counted Windows
// heartbeat threads establish target selection and continued execution after Stop.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include "yuzu_cmd/nx2_sampler.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace std::chrono_literals;
using Nx2Sampler::Options;
using Nx2Sampler::Session;
using Nx2Sampler::State;
int checks = 0;
void Check(bool condition, const char* text) {
    ++checks;
    if (!condition) throw std::runtime_error(text);
}
void Env(const char* name, const char* value) {
#ifdef _WIN32
    Check(_putenv_s(name, value ? value : "") == 0, "environment setup");
#else
    Check((value ? setenv(name, value, 1) : unsetenv(name)) == 0, "environment setup");
#endif
}
Options Base() { Options o; o.patterns = {"SamplerFixture*"}; return o; }
void Rejected(Options o, const char* message) {
    Session session;
    const auto status = session.Start(std::move(o));
    Check(status.state == State::Failed && status.error.find(message) != std::string::npos,
          "invalid option must fail before starting");
    session.Stop();
    Check(session.GetStatus().samples == 0, "invalid request captured nothing");
}
template<typename Predicate> void Wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(2ms);
    Check(predicate(), "bounded state wait");
}
void Validation() {
    for (const auto* name : {"EDEN_NX2_SAMPLE", "EDEN_NX2_SAMPLE_START",
                             "EDEN_NX2_SAMPLE_SECONDS", "EDEN_NX2_SAMPLE_OUT"}) Env(name, nullptr);
    Session disabled;
    Check(disabled.StartFromEnvironment().state == State::Disabled, "unset is disabled");
    disabled.Stop(); disabled.Stop();
    Check(disabled.GetStatus().state == State::Disabled, "disabled stop is idempotent");
    Check(disabled.Start(Base()).state == State::Failed, "single-start contract");
    auto o = Base(); o.delay_seconds = -1; Rejected(o, "delay");
    o = Base(); o.delay_seconds = 600.001; Rejected(o, "delay");
    o = Base(); o.delay_seconds = std::numeric_limits<double>::quiet_NaN(); Rejected(o, "delay");
    o = Base(); o.duration_seconds = 0; Rejected(o, "duration");
    o = Base(); o.duration_seconds = 120.001; Rejected(o, "duration");
    o = Base(); o.duration_seconds = std::numeric_limits<double>::infinity(); Rejected(o, "duration");
    o = Base(); o.patterns = {"a*b"}; Rejected(o, "patterns");
    o = Base(); o.patterns = {"**"}; Rejected(o, "patterns");
    o = Base(); o.patterns.resize(33, "a"); Rejected(o, "patterns");
    o = Base(); o.output_path.clear(); Rejected(o, "output");
    Env("EDEN_NX2_SAMPLE", "SamplerFixture*");
    Env("EDEN_NX2_SAMPLE_SECONDS", "1junk");
    Session malformed;
    Check(malformed.StartFromEnvironment().state == State::Failed, "numeric suffix rejected");
    Env("EDEN_NX2_SAMPLE_SECONDS", nullptr);
    Env("EDEN_NX2_SAMPLE", nullptr);
    Session sealed; sealed.Stop();
    Check(sealed.Start(Base()).state == State::Failed, "stop before start seals owner");
#ifndef _WIN32
    Session unsupported;
    const auto status = unsupported.Start(Base());
    Check(status.state == State::Failed && status.error.find("Windows") != std::string::npos,
          "Linux explicitly reports unsupported sampling");
#endif
}
#if defined(_WIN32) && defined(_MSC_VER) && defined(_M_X64)
void Native() {
    Check(SUCCEEDED(SetThreadDescription(GetCurrentThread(), L"SamplerOwner")), "owner name");
    std::atomic<unsigned long long> beats{};
    std::atomic<bool> ready{};
    std::jthread target{[&](std::stop_token stop) {
        SetThreadDescription(GetCurrentThread(), L"SamplerFixtureA");
        ready = true;
        while (!stop.stop_requested()) { ++beats; std::this_thread::yield(); }
    }};
    Wait([&] { return ready.load(); });
    auto o = Base(); o.delay_seconds = 600; o.duration_seconds = 1;
    Session delayed;
    Check(delayed.Start(o).state == State::Waiting, "delayed start");
    const auto start = std::chrono::steady_clock::now();
    delayed.Stop();
    Check(std::chrono::steady_clock::now() - start < 3s, "delay cancellation joins promptly");
    Check(delayed.GetStatus().state == State::Cancelled && delayed.GetStatus().samples == 0,
          "cancelled before collection");
    o.delay_seconds = 0; o.duration_seconds = 5; o.patterns = {"Sampler*"};
    Session active; active.Start(o);
    Wait([&] { return active.GetStatus().samples > 0 || active.GetStatus().state == State::Failed; });
    Check(active.GetStatus().state != State::Failed, "native capture succeeds");
    Check(active.GetStatus().targets == 1, "owner thread excluded despite matching prefix");
    active.Stop(); active.Stop();
    Check(active.GetStatus().state == State::Cancelled, "active cancellation");
    const auto before = beats.load();
    Wait([&] { return beats.load() > before + 10; });
    // A normal report followed by a fresh Session verifies released native handles.
    const auto dir = std::filesystem::temp_directory_path() /
                     ("eden-sampler-test-" + std::to_string(GetCurrentProcessId()));
    Check(std::filesystem::create_directory(dir), "exclusive test output directory");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{dir};
    o.patterns = {"SamplerFixtureA"}; o.duration_seconds = .05;
    o.output_path = (dir / "sample.txt").string();
    Session normal; normal.Start(o);
    Wait([&] { const auto s = normal.GetStatus().state; return s == State::Completed || s == State::Failed; });
    normal.Stop();
    const auto status = normal.GetStatus();
    Check(status.state == State::Completed && status.targets == 1 && status.samples > 0,
          "normal sampling completes");
    std::ifstream folded(o.output_path + ".folded");
    std::string line; std::size_t total = 0;
    while (std::getline(folded, line)) {
        Check(line.starts_with("SamplerFixtureA;"), "folded root is known target");
        total += std::stoull(line.substr(line.rfind(' ') + 1));
    }
    Check(total == status.samples, "folded counts equal independently observed samples");
    o.output_path = (dir / "missing" / "sample.txt").string();
    Session bad_output; bad_output.Start(o);
    Wait([&] { return bad_output.GetStatus().state == State::Failed; });
    bad_output.Stop();
    Check(bad_output.GetStatus().error.find("output") != std::string::npos, "output failure surfaced");
    const auto after = beats.load();
    Wait([&] { return beats.load() > after + 10; });
}
#endif
} // namespace
int main() {
    try {
        Validation();
#if defined(_WIN32) && defined(_MSC_VER) && defined(_M_X64)
        Native();
#endif
        std::printf("%d checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "after %d checks: %s\n", checks, e.what());
        return 1;
    }
}
