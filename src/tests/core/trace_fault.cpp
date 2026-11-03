// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <thread>

#include "core/debugger/trace_fault.h"

namespace {
using namespace Core::TraceFault;

void Require(bool condition, const std::source_location where = std::source_location::current()) {
    if (!condition) {
        std::cerr << "FAIL " << where.file_name() << ':' << where.line() << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool Contains(const std::string& value, std::string_view part) {
    return value.find(part) != std::string::npos;
}

struct Fixture {
    // Independent little-endian AArch64 frame records:
    // 0x8000 -> {next=0x8020, saved LR=0x401008}
    // 0x8020 -> {next=0, saved LR=0x501020}
    std::array<u8, 16> first{0x20, 0x80, 0, 0, 0, 0, 0, 0,
                             0x08, 0x10, 0x40, 0, 0, 0, 0, 0};
    std::array<u8, 16> second{0, 0, 0, 0, 0, 0, 0, 0,
                              0x20, 0x10, 0x50, 0, 0, 0, 0, 0};
    unsigned reads{};
    unsigned faults{};
    bool accept_fault{};
    bool second_readable{true};
    bool recursive_resolver{};
    Access last_access;

    Context MakeContext() {
        Context context;
        context.memory = this;
        context.process_id = 17;
        context.thread_id = 23;
        context.core_id = 2;
        context.pc = 0x401004;
        context.lr = 0x501008;
        context.fp = 0x8000;
        context.sp = 0x7ff0;
        context.pc_quality = Quality::Exact;
        context.registers_quality = Quality::Exact;
        context.user = this;
        context.read = [](void* user, u64 address, std::span<u8> output) {
            auto& fixture = *static_cast<Fixture*>(user);
            ++fixture.reads;
            Require(output.size() == 16);
            if (address == 0x8000) {
                std::copy(fixture.first.begin(), fixture.first.end(), output.begin());
                return true;
            }
            if (address == 0x8020 && fixture.second_readable) {
                std::copy(fixture.second.begin(), fixture.second.end(), output.begin());
                return true;
            }
            return false;
        };
        context.resolve = [](void* user, u64 process, u64 address) -> std::string {
            auto& fixture = *static_cast<Fixture*>(user);
            Require(process == 17);
            if (fixture.recursive_resolver) {
                Require(Contains(DescribeUnmapped(&fixture, {0, 8, AccessKind::Read}),
                                 "reason=recursive_diagnostic"));
            }
            switch (address) {
            case 0x401004: return "plugin.nro+0x1004 (plugin_read+0x4)";
            case 0x501008: return "main+0x1008";
            case 0x401008: return "plugin.nro+0x1008";
            case 0x501020: return "main+0x1020";
            default: return {};
            }
        };
        context.queue_guest_fault = [](void* user, const Context& at, const Access& access) noexcept {
            auto& fixture = *static_cast<Fixture*>(user);
            ++fixture.faults;
            Require(at.pc == 0x401004 && at.lr == 0x501008);
            fixture.last_access = access;
            return fixture.accept_fault;
        };
        return context;
    }
};
} // namespace

int main() {
    const Access access{0x8, 8, AccessKind::Read};
    Fixture fixture;
    auto context = fixture.MakeContext();
    Require(Contains(DescribeUnmapped(&fixture, access), "no_matching_guest_callback"));
    {
        const auto before_first = fixture.first;
        const auto before_second = fixture.second;
        AccessScope scope{context};
        const auto line = DescribeUnmapped(&fixture, access);
        Require(Contains(line, "process=17 thread=23 core=2"));
        Require(Contains(line, "PC=0x401004(plugin.nro+0x1004 (plugin_read+0x4)) pc_quality=exact"));
        Require(Contains(line, "LR=0x501008(main+0x1008) registers_quality=exact"));
        Require(Contains(line, "[0x401008(plugin.nro+0x1008), 0x501020(main+0x1020)]"));
        Require(Contains(line, "walk_stop=end fault=not_requested"));
        Require(fixture.reads == 2 && fixture.faults == 0);
        Require(fixture.first == before_first && fixture.second == before_second);
    }
    std::cout << "PASS exact attribution, process-aware module/symbol resolution, immutable frame records\n";

    {
        AccessScope scope{context};
        auto sampled = context;
        sampled.pc_quality = Quality::Sampled;
        sampled.registers_quality = Quality::Sampled;
        {
            AccessScope nested{sampled};
            const auto line = DescribeUnmapped(&fixture, access);
            Require(Contains(line, "pc_quality=sampled_not_fault_site"));
            Require(Contains(line, "registers_quality=sampled_not_fault_site"));
            int unrelated_memory{};
            Require(Contains(DescribeUnmapped(&unrelated_memory, access), "no_matching_guest_callback"));
        }
        Require(Contains(DescribeUnmapped(&fixture, access), "pc_quality=exact"));
        std::thread other([&] {
            Require(Contains(DescribeUnmapped(&fixture, access), "no_matching_guest_callback"));
        });
        other.join();
    }
    Require(Contains(DescribeUnmapped(&fixture, access), "no_matching_guest_callback"));
    std::cout << "PASS sampled labels, memory identity, nested restoration and thread-local isolation\n";

    {
        Fixture invalid;
        auto at = invalid.MakeContext();
        invalid.first[0] = 0; // next=0x8000: self-cycle, one valid saved LR then stop.
        auto trace = WalkBacktrace(at);
        Require(trace.count == 1 && trace.returns[0] == 0x401008);
        Require(trace.stop == WalkStop::InvalidFrame && invalid.reads == 1);
        at.fp = 0x8008;
        Require(WalkBacktrace(at).stop == WalkStop::InvalidFrame && invalid.reads == 1);
        at.fp = 0x8000;
        invalid.first[0] = 0x20;
        invalid.second_readable = false;
        trace = WalkBacktrace(at);
        Require(trace.count == 1 && trace.stop == WalkStop::Unreadable && invalid.reads == 3);
        at.fp = 0x200000; // Beyond the one-MiB stack window; no read.
        Require(WalkBacktrace(at).stop == WalkStop::InvalidFrame && invalid.reads == 3);
        at.registers_quality = Quality::Unavailable;
        Require(WalkBacktrace(at).stop == WalkStop::NoRegisters && invalid.reads == 3);
    }
    std::cout << "PASS cycle, alignment, stack-window and unreadable-frame boundaries\n";

    {
        unsigned reads{};
        auto at = context;
        at.user = &reads;
        at.read = [](void* user, u64 address, std::span<u8> output) {
            ++*static_cast<unsigned*>(user);
            Require(output.size() == 16);
            std::fill(output.begin(), output.end(), 0);
            // Nine readable records; walker must request only the first eight.
            Require(address >= 0x8000 && address <= 0x8080);
            output[0] = static_cast<u8>(address + 16);
            output[1] = 0x80;
            output[8] = 0x04;
            output[10] = 0x40;
            return true;
        };
        const auto trace = WalkBacktrace(at);
        Require(trace.count == 8 && reads == 8 && trace.stop == WalkStop::Limit);
        Require(std::all_of(trace.returns.begin(), trace.returns.end(),
                            [](u64 address) { return address == 0x400004; }));
        fixture.recursive_resolver = true;
        AccessScope scope{context};
        Require(Contains(DescribeUnmapped(&fixture, access), "walk_stop=end"));
        fixture.recursive_resolver = false;
    }
    std::cout << "PASS eight-frame budget and recursive-diagnostic suppression\n";

    {
        auto at = context;
        at.mode = Mode::Fault;
        Require(ValidateMode(Mode::Log, {}).empty());
        Require(!ValidateMode(at.mode, {}).empty());
        Require(RequestGuestFault(at, access) == FaultResult::UnsupportedBackend);
        at.capabilities = {true, true, false, false};
        Require(Contains(std::string{ValidateMode(at.mode, at.capabilities)}, "before result/writeback"));
        at.capabilities.abort_before_commit = true;
        Require(Contains(std::string{ValidateMode(at.mode, at.capabilities)}, "guest data-abort delivery"));
        // Synthetic contract only: these flags do not certify any emulator backend.
        at.capabilities.guest_exception_delivery = true;
        Require(ValidateMode(at.mode, at.capabilities).empty());
        at.pc_quality = Quality::Sampled;
        Require(RequestGuestFault(at, access) == FaultResult::InexactContext);
        at.pc_quality = Quality::Exact;
        at.queue_guest_fault = nullptr;
        Require(RequestGuestFault(at, access) == FaultResult::MissingHook);
        Require(fixture.faults == 0);
        at.queue_guest_fault = context.queue_guest_fault;
        {
            AccessScope scope{at};
            Require(Contains(DescribeUnmapped(&fixture, access), "fault=delivery_rejected"));
        }
        Require(fixture.faults == 1);
        fixture.accept_fault = true;
        {
            AccessScope scope{at};
            const Access exclusive{0x1230, 16, AccessKind::WriteExclusive};
            Require(Contains(DescribeUnmapped(&fixture, exclusive), "fault=queued_not_yet_delivered"));
            Require(fixture.last_access.address == 0x1230 && fixture.last_access.size == 16 &&
                    fixture.last_access.kind == AccessKind::WriteExclusive);
        }
        Require(fixture.faults == 2);
    }
    std::cout << "PASS unsupported/inexact/missing-hook refusal, rejection and explicit queued-only result\n";
}
