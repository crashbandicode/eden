// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <thread>
#include "core/debugger/guest_trace.h"

namespace {
void Require(bool value, const std::source_location where = std::source_location::current()) {
    if (!value) {
        std::cerr << "FAIL " << where.line() << '\n';
        std::exit(1);
    }
}
}
int main() {
    using Trace = Core::GuestTrace;
    using namespace std::chrono_literals;
    const auto now = Trace::Clock::now();
    const Trace::MemoryReader no_memory = [](u64, std::span<u8>) { return false; };
    Trace::Registers regs{};
    regs[32] = 0x1000;
    regs[30] = 0x2000;
    regs[31] = 0x8000;
    {
        Trace trace;
        Trace::Config config;
        config.address = 0x1000;
        u64 first{}, second{};
        Require(trace.Arm(7, config, first).empty());
        config.address = 0x1010;
        Require(trace.Arm(7, config, second).empty());
        for (u64 i = 0; i < 5; ++i) {
            regs[0] = i;
            trace.Hit(7, 2, 0, regs, no_memory, now);
            trace.AdvanceFrame();
        }
        regs[32] = 0x1010;
        trace.Hit(7, 2, 1, regs, no_memory, now);
        const auto result = trace.Read(0);
        Require(result.records.size() == 6 && result.records[4].frame == 4);
        Require(result.records[5].point_id == second && result.records[5].core_id == 1);
        Require(trace.List()[0].hits == 5 && trace.List()[1].hits == 1);
        trace.Disarm(first);
        Require(!trace.ShouldInstrument(7, 0x1000) && trace.ShouldInstrument(7, 0x1010));
        trace.EndProcess();
        trace.BeginProcess(8);
        Require(!trace.ShouldInstrument(8, 0x1010));
    }
    std::cout << "PASS concurrent persistent addresses, ordering, vsync frame, explicit unload lifecycle\n";
    regs[32] = 0x1000;
    {
        Trace trace;
        Trace::Config config;
        config.target = "plugin!hook";
        u64 id{};
        Require(trace.Arm(0, config, id).empty());
        trace.BeginProcess(7);
        Require(!trace.ShouldInstrument(7, 0x1000));
        const auto resolver = [](const std::string& target) -> std::optional<u64> {
            Require(target == "plugin!hook");
            return 0x1000;
        };
        Require(trace.ResolveTargets(7, 1, resolver) == std::vector<u64>{0x1000});
        // A translator may observe resolution first and ignore the return value.
        // The all-core owner must still receive the invalidation exactly once.
        Require(trace.ResolveTargets(7, 1, resolver).empty());
        Require(trace.TakeInvalidations().addresses == std::vector<u64>{0x1000});
        Require(trace.TakeInvalidations().addresses.empty());
        Require(trace.ShouldInstrument(7, 0x1000));
        trace.ResolveTargets(7, 2, [](const auto&) -> std::optional<u64> { return {}; });
        Require(!trace.ShouldInstrument(7, 0x1000));
        trace.ResolveTargets(7, 3, [](const auto&) -> std::optional<u64> { return 0x4000; });
        Require(trace.ShouldInstrument(7, 0x4000) && !trace.ShouldInstrument(8, 0x4000));
    }
    std::cout << "PASS preboot pending resolution, unload and ASLR relocation\n";
    {
        Trace trace;
        Trace::Config config;
        config.address = 0x1000;
        config.condition = "x0 == 3";
        config.counters_only = true;
        config.max_hits = 2;
        u64 id{};
        Require(trace.Arm(7, config, id).empty());
        for (u64 value : {1, 3, 2, 3, 3}) {
            regs[0] = value;
            trace.Hit(7, 2, 0, regs, no_memory, now);
        }
        const auto point = trace.List().front();
        Require(point.hits == 4 && point.matched == 2 && point.recorded == 0);
        Require(point.state == Trace::State::Completed && trace.Read(0).records.empty());
    }
    std::cout << "PASS counter-only conditions and exact max hit completion\n";
    {
        Trace trace;
        Trace::Config config;
        config.address = 0x1000;
        config.rate_limit = 2;
        u64 id{};
        Require(trace.Arm(7, config, id).empty());
        for (int i = 0; i < 5; ++i) trace.Hit(7, 2, 0, regs, no_memory, now);
        trace.Hit(7, 2, 0, regs, no_memory, now + 1s);
        const auto point = trace.List().front();
        Require(point.hits == 6 && point.recorded == 3 && point.dropped == 3);
    }
    std::cout << "PASS rate-limited records report loss without losing hit counts\n";
    {
        Trace trace;
        Trace::Config config;
        config.address = 0x1000;
        config.on_return = true;
        config.max_hits = 1;
        u64 id{};
        Require(trace.Arm(7, config, id).empty());
        Require(trace.Hit(7, 2, 0, regs, no_memory, now) == std::vector<u64>{0x2000});
        regs[32] = 0x2000;
        regs[0] = 42;
        trace.Hit(7, 3, 1, regs, no_memory, now); // Same PC in a different thread.
        Require(trace.Read(0).records.size() == 1);
        trace.Hit(7, 2, 1, regs, no_memory, now);
        const auto records = trace.Read(0).records;
        Require(records.size() == 2 && records.back().returning);
        Require(records.back().registers.front() == std::pair<u8, u64>{0, 42});
        Require(!trace.ShouldInstrument(7, 0x2000));
    }
    std::cout << "PASS return pairing uses thread and stack, records return x0 after max entry count\n";
    regs[32] = 0x1000;
    {
        Trace trace;
        Trace::Config config;
        config.address = 0x1000;
        config.on_return = true;
        config.max_hits = 1;
        u64 a{}, b{};
        Require(trace.Arm(7, config, a).empty());
        config.address = 0x1010;
        Require(trace.Arm(7, config, b).empty());
        trace.Hit(7, 2, 0, regs, no_memory, now);
        Require(trace.Disarm(b));
        Require(trace.HasPoints() && trace.ShouldInstrument(7, 0x2000));
        regs[32] = 0x2000;
        trace.Hit(7, 2, 0, regs, no_memory, now);
        Require(trace.Read(0).records.size() == 2 && !trace.HasPoints());
    }
    std::cout << "PASS unrelated disarm preserves completed-point outstanding return\n";
    regs[32] = 0x1000;
    {
        Trace trace;
        Trace::Config config;
        config.address = 0x1000;
        config.on_return = true;
        u64 id{};
        Require(trace.Arm(7, config, id).empty());
        trace.Hit(7, 2, 0, regs, no_memory, now);
        trace.Hit(7, 2, 0, regs, no_memory, now); // First call unwound; reused SP/LR.
        regs[32] = 0x2000;
        trace.Hit(7, 2, 0, regs, no_memory, now);
        Require(trace.Read(0).records.size() == 3);
        Require(trace.List().front().abandoned_returns == 1);
    }
    std::cout << "PASS reused invocation tuple accounts abandoned call rather than duplicating return\n";
    regs[32] = 0x1000;
    for (const auto max_hits : {u64{0}, u64{1}}) {
        Trace trace;
        Trace::Config config;
        config.address = 0x1000;
        config.on_return = true;
        config.condition = "x0 == 3";
        config.max_hits = max_hits;
        u64 id{};
        Require(trace.Arm(7, config, id).empty());
        regs[0] = 3;
        trace.Hit(7, 2, 0, regs, no_memory, now);
        Require(trace.ShouldInstrument(7, 0x1000)); // Including a completed point with pending LR.
        regs[0] = 4;
        trace.Hit(7, 2, 0, regs, no_memory, now); // Rejected or already maxed entry retires A.
        regs[32] = 0x2000;
        trace.Hit(7, 2, 0, regs, no_memory, now);
        Require(trace.Read(0).records.size() == 1 && trace.List().front().abandoned_returns == 1);
        regs[32] = 0x1000;
    }
    std::cout << "PASS rejected or maxed reused invocation cannot manufacture an old return\n";
    {
        Trace trace;
        Trace::Config config;
        config.address = 0x1000;
        config.rate_limit = 10000;
        u64 id{};
        Require(trace.Arm(7, config, id).empty());
        for (size_t i = 0; i < Trace::MaxRecords + 17; ++i) trace.Hit(7, 2, 0, regs, no_memory, now);
        const auto first = trace.Read(0);
        Require(first.records.size() == 128 && first.lost == 17 && first.evicted == 17);
        const auto second = trace.Read(first.next_sequence);
        Require(second.lost == 0 && second.records.front().sequence == first.next_sequence + 1);
    }
    std::cout << "PASS bounded ring and independent cursor overrun accounting\n";
    {
        Trace trace;
        Trace::Config config;
        config.address = 0x1000;
        config.captures = {{"x1", "raw", 16}};
        u64 id{};
        Require(trace.Arm(7, config, id).empty());
        trace.Hit(7, 2, 0, regs, [](u64, std::span<u8> bytes) {
            Require(bytes.size() == 16);
            std::fill(bytes.begin(), bytes.end(), 0xab);
            return true;
        }, now);
        Require(trace.Read(0).records.front().captures.front().bytes.size() == 16);
        Require(trace.Read(0).records.front().captures.front().spec.type == "bytes");
        trace.EndProcess();
        trace.Clear();
        const auto cleared = trace.Read(0);
        Require(cleared.records.empty() && cleared.lost == 1 && cleared.next_sequence == 1);
        Require(trace.Read(cleared.next_sequence).lost == 0);
        Require(trace.Arm(0, config, id).empty());
        trace.BeginProcess(8);
        Require(trace.ShouldInstrument(8, 0x1000));
    }
    std::cout << "PASS raw alias captures bytes, empty-ring clear loss and fresh process preboot arm\n";
    {
        Trace trace;
        Trace::Config config;
        config.address = 0x1000;
        config.captures = {{"[x1+8]", "u32", 4}};
        u64 id{};
        Require(trace.Arm(7, config, id).empty());
        regs[1] = 0x3000;
        const auto read = [](u64 address, std::span<u8> bytes) {
            std::fill(bytes.begin(), bytes.end(), 0);
            if (address == 0x3008 && bytes.size() == 8) { bytes[1] = 0x40; return true; }
            if (address == 0x4000 && bytes.size() == 4) { bytes[0] = 0xab; return true; }
            return false;
        };
        trace.Hit(7, 2, 0, regs, read, now);
        const auto captured = trace.Read(0).records.front().captures.front();
        Require(captured.address == 0x4000 && captured.bytes == std::vector<u8>({0xab, 0, 0, 0}));
        trace.Hit(7, 2, 0, regs, no_memory, now);
        Require(!trace.Read(1).records.front().captures.front().error.empty());
    }
    std::cout << "PASS bounded pointer capture and failed-read isolation\n";
    {
        Trace trace;
        Trace::Config config;
        config.address = 0x1000;
        config.counters_only = true;
        u64 id{};
        Require(trace.Arm(7, config, id).empty());
        std::vector<std::thread> workers;
        for (u32 i = 0; i < 4; ++i) workers.emplace_back([&, i] {
            for (size_t n = 0; n < 1000; ++n) trace.Hit(7, i + 1, i, regs, no_memory, now);
        });
        for (auto& worker : workers) worker.join();
        Require(trace.List().front().hits == 4000);
        auto invalid = config;
        invalid.registers = {99};
        Require(!trace.Arm(7, invalid, id).empty() && trace.List().size() == 1);
        for (size_t i = 1; i < Trace::MaxPoints; ++i) Require(trace.Arm(7, config, id).empty());
        Require(trace.Arm(7, config, id) == "trace_limit");
    }
    std::cout << "PASS cross-core counter consistency and transactional resource limits\n";
}
