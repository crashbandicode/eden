// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "core/debugger/guest_tracepoint.h"

namespace Core {

// Host-only observation state. CPU threads never perform filesystem I/O here.
class GuestTrace {
public:
    using Registers = GuestTracepoint::Registers;
    using MemoryReader = GuestTracepoint::MemoryReader;
    using Clock = std::chrono::steady_clock;
    using Resolver = std::function<std::optional<u64>(const std::string&)>;
    using Describer = std::function<std::string(u64)>;
    static constexpr size_t MaxPoints = 256;
    static constexpr size_t MaxRecords = 1024;
    static constexpr size_t MaxCaptureBytes = 4096;

    struct CaptureSpec {
        std::string expression;
        std::string type{"bytes"};
        u32 size{};
    };
    struct Config {
        u64 address{};              // Either absolute address or a target, never both.
        std::string target;         // module+offset or module!symbol
        std::vector<u8> registers{0, 30, 32};
        std::vector<CaptureSpec> captures;
        std::string condition;
        bool counters_only{};
        bool on_return{};
        bool backtrace{};
        u64 max_hits{};             // 0: until disarmed/unload
        u32 rate_limit{1000};       // records/second; counting is never rate limited
    };
    enum class State { Pending, Armed, Completed, Disarmed };
    struct Point {
        u64 id{}, process_id{}, address{};
        Config config;
        State state{State::Pending};
        u64 hits{}, matched{}, recorded{}, dropped{}, return_overflow{}, abandoned_returns{};
    };
    struct Capture {
        CaptureSpec spec;
        u64 address{};
        std::vector<u8> bytes;
        std::string error;
    };
    struct Record {
        u64 sequence{}, point_id{}, process_id{}, thread_id{}, frame{}, wall_time_ns{};
        u32 core_id{};
        bool returning{};
        std::vector<std::pair<u8, u64>> registers;
        std::vector<Capture> captures;
        std::vector<u64> backtrace;
        std::vector<std::pair<u64, std::string>> locations;
    };
    struct Batch {
        std::vector<Record> records;
        u64 next_sequence{}, lost{}, evicted{};
    };

    GuestTrace();
    ~GuestTrace();
    GuestTrace(const GuestTrace&) = delete;
    GuestTrace& operator=(const GuestTrace&) = delete;

    std::string Arm(u64 process_id, const Config& config, u64& id);
    bool Disarm(u64 id); // 0: all; retains counters
    void Clear();       // only legal with guest stopped, enforced by control adapter
    void BeginProcess(u64 process_id);
    void EndProcess();
    std::vector<Point> List() const;
    std::vector<u64> ResolveTargets(u64 process_id, u64 revision, const Resolver& resolve);
    struct Invalidations { bool all{}; std::vector<u64> addresses; };
    Invalidations TakeInvalidations(); // Only the all-core invalidation owner consumes this.
    bool ShouldInstrument(u64 process_id, u64 address) const;
    // Returned addresses are newly required return traps to invalidate on all cores.
    std::vector<u64> Hit(u64 process_id, u64 thread_id, u32 core_id,
                         const Registers& registers, const MemoryReader& read,
                         Clock::time_point now = Clock::now(), const Describer& describe = {});
    Batch Read(u64 after, size_t limit = 128) const;
    void SetMainThread(u64 thread_id);
    void AdvanceFrame() { frame.fetch_add(1, std::memory_order_relaxed); }
    bool HasPoints() const { return active.load(std::memory_order_acquire); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::atomic<bool> active{};
    std::atomic<u64> frame{};
};

} // namespace Core
