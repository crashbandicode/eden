// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "common/common_types.h"

namespace Core::TraceFault {

enum class Mode { Log, Fault };
enum class Quality { Unavailable, Sampled, Exact };
enum class AccessKind { Read, Write, WriteExclusive };

struct Access {
    u64 address{}; // Address supplied to Memory, before its existing upper-bit mask.
    std::size_t size{};
    AccessKind kind{};
};

// These are backend guarantees, not user-selectable options. An emulator stop or
// debugger watchpoint is NOT guest_exception_delivery. Defaults reject fault mode.
struct BackendCapabilities {
    bool precise_pc{};
    bool precise_registers{};
    bool abort_before_commit{};
    bool guest_exception_delivery{};
};

// Return an actionable error before accepting configuration / starting the CPU.
[[nodiscard]] std::string_view ValidateMode(Mode mode, const BackendCapabilities& capabilities);

struct Context {
    // Scope only the CPU memory callback, never Run(), an SVC, or a host operation.
    // Identity must be the Memory object whose guest address space is being used.
    const void* memory{};
    u64 process_id{};
    std::optional<u64> thread_id;
    u32 core_id{};
    u64 pc{};
    u64 lr{};
    u64 fp{};
    u64 sp{};
    Quality pc_quality{Quality::Unavailable};
    Quality registers_quality{Quality::Unavailable};
    Mode mode{Mode::Log};
    BackendCapabilities capabilities;

    void* user{};
    // Optional lazy sampling, invoked only on an unmapped slow path. It must
    // label JIT callback accessors Sampled, never Exact.
    void (*sample)(void* user, Context& context){};
    // Bounded, non-logging CPU-visible read. Must not use normal ReadN/ReadBlock,
    // flush the GPU, change guest state, or retain the span.
    bool (*read)(void* user, u64 address, std::span<u8> bytes){};
    // Optional process-aware module+offset and symbol formatter. Return empty for
    // unknown addresses. Must use registered modules, not scan guest memory.
    std::string (*resolve)(void* user, u64 process_id, u64 address){};
    // Queue an architectural fault and arrange immediate abort-before-commit.
    // true means QUEUED, not delivered. Never throw/unwind across generated code.
    // The backend owns pending-fault storage and delivery after Run() returns.
    bool (*queue_guest_fault)(void* user, const Context& context, const Access& access) noexcept{};
};

class AccessScope {
public:
    explicit AccessScope(const Context& context);
    ~AccessScope();
    AccessScope(const AccessScope&) = delete;
    AccessScope& operator=(const AccessScope&) = delete;

private:
    Context context;
    const Context* previous;
};

enum class FaultResult {
    NotRequested,
    UnsupportedBackend,
    InexactContext,
    MissingHook,
    Rejected,
    Queued,
};

[[nodiscard]] FaultResult RequestGuestFault(const Context& context, const Access& access) noexcept;

constexpr std::size_t MaxFrames = 8;
enum class WalkStop { End, NoRegisters, NoReader, InvalidFrame, Unreadable, Limit };
struct Backtrace {
    // Saved return addresses, not inferred call-site PCs (no unconditional -4).
    std::array<u64, MaxFrames> returns{};
    std::size_t count{};
    WalkStop stop{WalkStop::End};
};

// AArch64 frame records only. At most MaxFrames * 16 bytes, with no guest writes.
[[nodiscard]] Backtrace WalkBacktrace(const Context& context);

// Called only on the unmapped slow path. Appended to the SAME Unmapped ReadN /
// WriteN line. Missing/mismatched context and recursive diagnostics are explicit.
// The Memory API still returns a placeholder on a failed read: only a backend
// satisfying the contract above may prevent that value being committed by the JIT.
[[nodiscard]] std::string DescribeUnmapped(const void* memory, const Access& access);

} // namespace Core::TraceFault
