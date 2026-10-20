// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>

#include "core/debugger/guest_tracepoint.h"

namespace Core {

std::string GuestTracepoint::Arm(u64 process_id, const Config& config, Clock::time_point now) {
    if (!config.address || config.address % 4 ||
        config.address > std::numeric_limits<u64>::max() - 4 ||
        config.registers.empty() || config.registers.size() > RegisterCount ||
        config.memory_size > MaxMemoryBytes || config.timeout_ms < 1 ||
        config.timeout_ms > MaxTimeoutMs ||
        (config.memory_register && *config.memory_register >= RegisterCount) ||
        config.memory_offset < -1048576 || config.memory_offset > 1048576) {
        return "invalid_tracepoint";
    }
    std::array<bool, RegisterCount> seen{};
    for (const auto reg : config.registers) {
        if (reg >= RegisterCount || seen[reg]) {
            return "invalid_tracepoint";
        }
        seen[reg] = true;
    }
    std::scoped_lock lock{mutex};
    Expire(now);
    if (result.state == State::Armed) {
        return "tracepoint_busy";
    }
    const u64 next_id = result.id + 1;
    result = {};
    result.id = next_id;
    result.state = State::Armed;
    result.config = config;
    result.process_id = process_id;
    deadline = now + std::chrono::milliseconds(config.timeout_ms);
    armed_process.store(process_id, std::memory_order_relaxed);
    armed_address.store(config.address, std::memory_order_release);
    return {};
}

bool GuestTracepoint::ShouldInstrument(u64 process_id, u64 address) const {
    return address != 0 && armed_address.load(std::memory_order_acquire) == address &&
           armed_process.load(std::memory_order_relaxed) == process_id;
}

void GuestTracepoint::Expire(Clock::time_point now) {
    if (result.state == State::Armed && now >= deadline) {
        armed_address.store(0, std::memory_order_release);
        result.state = State::Expired;
    }
}

void GuestTracepoint::Capture(u64 process_id, u64 thread_id, const Registers& registers,
                             const MemoryReader& read, Clock::time_point now) {
    std::scoped_lock lock{mutex};
    Expire(now);
    if (result.state != State::Armed || result.process_id != process_id ||
        result.config.address != registers[32]) {
        return;
    }
    // Disarm before any memory access. Other CPU cores may already be executing
    // an old translated tracepoint; those events only resume, never recapture.
    armed_address.store(0, std::memory_order_release);
    result.state = State::Captured;
    result.thread_id = thread_id;
    for (const auto reg : result.config.registers) {
        result.values.push_back(registers[reg]);
    }
    if (!result.config.memory_size) {
        return;
    }
    const u64 base = result.config.memory_register ? registers[*result.config.memory_register]
                                                  : result.config.memory_address;
    const s64 offset = result.config.memory_offset;
    const u64 magnitude = static_cast<u64>(offset < 0 ? -offset : offset);
    if ((offset < 0 && base < magnitude) ||
        (offset >= 0 && base > std::numeric_limits<u64>::max() - magnitude)) {
        result.memory_error = "address_overflow";
        return;
    }
    result.memory_address = offset < 0 ? base - magnitude : base + magnitude;
    if (result.memory_address > std::numeric_limits<u64>::max() - result.config.memory_size) {
        result.memory_error = "address_overflow";
        return;
    }
    result.memory.resize(result.config.memory_size);
    if (!read(result.memory_address, result.memory)) {
        result.memory.clear();
        result.memory_error = "memory_unavailable";
    }
}

GuestTracepoint::Result GuestTracepoint::Status(Clock::time_point now) {
    std::scoped_lock lock{mutex};
    Expire(now);
    auto copy = result;
    if (copy.state == State::Armed) {
        copy.remaining_ms = static_cast<u32>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    }
    return copy;
}

void GuestTracepoint::Cancel() {
    std::scoped_lock lock{mutex};
    armed_address.store(0, std::memory_order_release);
    if (result.state == State::Armed) {
        result.state = State::Cancelled;
    }
}

void GuestTracepoint::Poll() {
    std::scoped_lock lock{mutex};
    Expire(Clock::now());
}

} // namespace Core
