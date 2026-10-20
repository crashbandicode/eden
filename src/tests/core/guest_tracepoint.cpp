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
