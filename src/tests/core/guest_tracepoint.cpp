// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <source_location>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

#include "core/debugger/guest_tracepoint.h"

namespace {
void Require(bool condition, const std::source_location where = std::source_location::current()) {
    if (!condition) {
        std::cerr << "FAIL " << where.file_name() << ':' << where.line() << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main() {
    using Trace = Core::GuestTracepoint;
    using namespace std::chrono_literals;
    const auto now = Trace::Clock::now();
    const auto config = [] {
        Trace::Config result;
        result.address = 0x1000;
        result.registers = {0, 30, 31, 32, 33};
        result.memory_register = 1;
        result.memory_offset = -4;
        result.memory_size = 8;
        result.timeout_ms = 100;
        return result;
    }();
    Trace::Registers registers{};
    registers[0] = 123;
    registers[1] = 0x2004;
    registers[30] = 0x1122334455667788;
    registers[31] = 0x3000;
    registers[32] = 0x1000;
    registers[33] = 0xa0000000;
    unsigned reads{};
    const auto read = [&](u64 address, std::span<u8> bytes) {
        ++reads;
        Require(address == 0x2000 && bytes.size() == 8);
        const std::array<u8, 8> expected{8, 6, 7, 5, 3, 0, 9, 1};
        std::copy(expected.begin(), expected.end(), bytes.begin());
        return true;
    };
    {
        Trace trace;
        Require(trace.Arm(4, config, now).empty());
        Require(trace.ShouldInstrument(4, 0x1000));
        Require(!trace.ShouldInstrument(5, 0x1000));
        trace.Capture(4, 7, registers, read, now + 1ms);
        const auto result = trace.Status(now + 2ms);
        Require(result.state == Trace::State::Captured && result.thread_id == 7);
        Require((result.values == std::vector<u64>{123, 0x1122334455667788, 0x3000, 0x1000, 0xa0000000}));
        Require((result.memory == std::vector<u8>{8, 6, 7, 5, 3, 0, 9, 1}));
        trace.Capture(4, 8, registers, read, now + 3ms);
        Require(reads == 1 && !trace.ShouldInstrument(4, 0x1000));
    }
    std::cout << "PASS selected registers, indirect bytes, exactly one capture\n";
    {
        Trace trace;
        Require(trace.Arm(4, config, now).empty());
        const auto before = trace.Status(now);
        auto bad = config;
        bad.registers = {34};
        Require(trace.Arm(4, bad, now) == "invalid_tracepoint");
        bad.registers = {0, 0};
        Require(trace.Arm(4, bad, now) == "invalid_tracepoint");
        bad = config;
        bad.memory_size = Trace::MaxMemoryBytes + 1;
        Require(trace.Arm(4, bad, now) == "invalid_tracepoint");
        bad = config;
        bad.address += 1;
        Require(trace.Arm(4, bad, now) == "invalid_tracepoint");
        bad = config;
        bad.timeout_ms = 0;
        Require(trace.Arm(4, bad, now) == "invalid_tracepoint");
        Require(trace.Arm(4, config, now) == "tracepoint_busy");
        const auto after = trace.Status(now);
        Require(after.id == before.id && after.state == Trace::State::Armed);
    }
    std::cout << "PASS invalid input and busy refusal preserve active state\n";
    {
        Trace trace;
        Require(trace.Arm(4, config, now).empty());
        trace.Capture(4, 9, registers, read, now + 100ms);
        Require(trace.Status(now + 100ms).state == Trace::State::Expired);
        Require(!trace.ShouldInstrument(4, 0x1000) && reads == 1);
        Require(trace.Arm(4, config, now + 101ms).empty());
        trace.Cancel();
        trace.Cancel();
        trace.Capture(4, 9, registers, read, now + 102ms);
        Require(trace.Status(now + 102ms).state == Trace::State::Cancelled && reads == 1);
    }
    std::cout << "PASS deadline and cancellation suppress capture\n";
    {
        Trace::Config defaults;
        Require(defaults.timeout_ms == 5000);

        unsigned long_reads{};
        const auto read_long = [&](u64 address, std::span<u8> bytes) {
            ++long_reads;
            Require(address == 0x2000 && bytes.size() == 8);
            std::fill(bytes.begin(), bytes.end(), 0xa5);
            return true;
        };
        const std::array<u32, 2> timeout_values{300000, 600000};
        const std::array<u32, 2> late_capture_ms{299999, 599999};
        for (std::size_t i = 0; i < timeout_values.size(); ++i) {
            Trace trace;
            auto long_config = config;
            long_config.timeout_ms = timeout_values[i];
            Require(trace.Arm(4, long_config, now).empty());

            const auto past_old_limit = trace.Status(now + 30001ms);
            Require(past_old_limit.state == Trace::State::Armed);
            Require(past_old_limit.remaining_ms == timeout_values[i] - 30001);
            Require(trace.ShouldInstrument(4, 0x1000));

            const auto late = now + std::chrono::milliseconds{
                static_cast<std::chrono::milliseconds::rep>(late_capture_ms[i])};
            const auto just_before_deadline = trace.Status(late);
            Require(just_before_deadline.state == Trace::State::Armed &&
                    just_before_deadline.remaining_ms == 1);
            const auto reads_before_capture = long_reads;
            trace.Capture(4, 12, registers, read_long, late);
            const auto captured = trace.Status(late);
            Require(captured.state == Trace::State::Captured && captured.thread_id == 12);
            Require(captured.values == std::vector<u64>({123, 0x1122334455667788, 0x3000, 0x1000, 0xa0000000}));
            Require(captured.memory == std::vector<u8>(8, 0xa5));
            Require(long_reads == reads_before_capture + 1);
        }
    }
    std::cout << "PASS five- and ten-minute deadlines stay armed and capture before expiry\n";
    {
        auto long_config = config;
        long_config.timeout_ms = 600000;
        Trace trace;
        Require(trace.Arm(4, long_config, now).empty());
        const auto before_invalid = trace.Status(now + 30001ms);
        Require(before_invalid.state == Trace::State::Armed && before_invalid.remaining_ms == 569999);

        auto too_long = long_config;
        too_long.timeout_ms = 600001;
        Require(trace.Arm(4, too_long, now + 30001ms) == "invalid_tracepoint");
        auto zero_timeout = long_config;
        zero_timeout.timeout_ms = 0;
        Require(trace.Arm(4, zero_timeout, now + 30001ms) == "invalid_tracepoint");

        const auto after_invalid = trace.Status(now + 30001ms);
        Require(after_invalid.id == before_invalid.id && after_invalid.process_id == before_invalid.process_id);
        Require(after_invalid.state == Trace::State::Armed && after_invalid.config.address == long_config.address);
        Require(after_invalid.config.registers == long_config.registers);
        Require(after_invalid.config.memory_address == long_config.memory_address);
        Require(after_invalid.config.memory_register == long_config.memory_register);
        Require(after_invalid.config.memory_offset == long_config.memory_offset);
        Require(after_invalid.config.memory_size == long_config.memory_size);
        Require(after_invalid.config.timeout_ms == long_config.timeout_ms);
        Require(after_invalid.remaining_ms == before_invalid.remaining_ms && after_invalid.remaining_ms == 569999);
        const auto last_millisecond = trace.Status(now + 599999ms);
        Require(last_millisecond.state == Trace::State::Armed && last_millisecond.remaining_ms == 1);

        const auto reads_before_expiry = reads;
        trace.Capture(4, 13, registers, read, now + 600000ms);
        const auto expired = trace.Status(now + 600000ms);
        Require(expired.state == Trace::State::Expired && expired.remaining_ms == 0);
        Require(reads == reads_before_expiry && !trace.ShouldInstrument(4, 0x1000));

        Trace cancelled;
        Require(cancelled.Arm(4, long_config, now).empty());
        const auto active_before_cancel = cancelled.Status(now + 30001ms);
        Require(active_before_cancel.state == Trace::State::Armed && active_before_cancel.remaining_ms == 569999);
        cancelled.Cancel();
        const auto reads_before_cancelled_hit = reads;
        cancelled.Capture(4, 14, registers, read, now + 30002ms);
        const auto cancelled_result = cancelled.Status(now + 30002ms);
        Require(cancelled_result.state == Trace::State::Cancelled && cancelled_result.id == active_before_cancel.id);
        Require(reads == reads_before_cancelled_hit && !cancelled.ShouldInstrument(4, 0x1000));
    }
    std::cout << "PASS exact maximum expiry, rejected timeout preservation, and cancellation\n";
    {
        Trace trace;
        Require(trace.Arm(4, config, now).empty());
        std::atomic<unsigned> concurrent_reads{};
        std::vector<std::thread> workers;
        for (unsigned i = 0; i < 8; ++i) {
            workers.emplace_back([&, i] {
                auto local = registers;
                local[0] = i + 10;
                trace.Capture(4, i + 10, local, [&](u64, std::span<u8> bytes) {
                    ++concurrent_reads;
                    std::fill(bytes.begin(), bytes.end(), static_cast<u8>(i));
                    return true;
                }, now + 1ms);
            });
        }
        for (auto& worker : workers) worker.join();
        const auto result = trace.Status(now + 2ms);
        Require(concurrent_reads == 1 && result.values[0] == result.thread_id);
        Require(result.memory.front() == result.thread_id - 10);
    }
    std::cout << "PASS competing hits publish one consistent capture\n";
    {
        Trace trace;
        Require(trace.Arm(4, config, now).empty());
        trace.Capture(4, 1, registers, [](u64, std::span<u8> bytes) {
            bytes[0] = 42;
            return false;
        }, now);
        auto result = trace.Status(now);
        Require(result.state == Trace::State::Captured && result.memory.empty());
        Require(result.memory_error == "memory_unavailable");
        auto overflow = config;
        overflow.memory_register.reset();
        overflow.memory_address = std::numeric_limits<u64>::max() - 2;
        overflow.memory_offset = 4;
        Require(trace.Arm(4, overflow, now).empty());
        trace.Capture(4, 1, registers, read, now);
        result = trace.Status(now);
        Require(result.memory_error == "address_overflow" && reads == 1);
    }
    std::cout << "PASS unavailable memory and overflow do not expose partial bytes\n";
    {
        Trace trace;
        Require(trace.Arm(4, config, now).empty());
        trace.Cancel();
        auto next = config;
        next.address = 0x1010;
        Require(trace.Arm(5, next, now).empty());
        trace.Capture(4, 1, registers, read, now);
        Require(trace.Status(now).state == Trace::State::Armed && reads == 1);
        registers[32] = 0x1010;
        trace.Capture(5, 2, registers, read, now);
        Require(trace.Status(now).state == Trace::State::Captured && reads == 2);
    }
    std::cout << "PASS stale translated events cannot capture a different request\n";
}
