// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Core {

// Host diagnostic state only. It never owns a guest thread, patches guest memory,
// or leaves a thread suspended while a client is absent.
class GuestTracepoint {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr u32 MaxMemoryBytes = 4096;
    static constexpr u32 MaxTimeoutMs = 600000; // 10 minutes
    static constexpr u8 RegisterCount = 34; // x0..x30, sp, pc, pstate
    using Registers = std::array<u64, RegisterCount>;
    using MemoryReader = std::function<bool(u64, std::span<u8>)>;

    enum class State { Idle, Armed, Captured, Expired, Cancelled };
    struct Config {
        u64 address{};
        std::vector<u8> registers;
        u64 memory_address{};
        std::optional<u8> memory_register;
        s64 memory_offset{};
        u32 memory_size{};
        u32 timeout_ms{5000};
    };
    struct Result {
        u64 id{};
        State state{State::Idle};
        Config config;
        u64 process_id{};
        u64 thread_id{};
        std::vector<u64> values;
        u64 memory_address{};
        std::vector<u8> memory;
        std::string memory_error;
        u32 remaining_ms{};
    };

    // Empty string means armed. Invalid input and a busy tracepoint leave the
    // previous result untouched. Only one bounded result is retained.
    std::string Arm(u64 process_id, const Config& config, Clock::time_point now = Clock::now());
    bool ShouldInstrument(u64 process_id, u64 address) const;
    void Capture(u64 process_id, u64 thread_id, const Registers& registers,
                 const MemoryReader& read, Clock::time_point now = Clock::now());
    Result Status(Clock::time_point now = Clock::now());
    void Poll();
    void Cancel();

private:
    void Expire(Clock::time_point now); // mutex held
    std::mutex mutex;
    Result result;
    Clock::time_point deadline{};
    std::atomic<u64> armed_address{};
    std::atomic<u64> armed_process{};
};

} // namespace Core
