// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <charconv>
#include <limits>

#include "core/debugger/trace_fault.h"

namespace Core::TraceFault {
namespace {
thread_local const Context* current_context{};
thread_local bool describing{};

std::string Hex(u64 value) {
    std::array<char, 16> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
    return "0x" + std::string(buffer.data(), result.ptr);
}

std::string_view QualityName(Quality quality) {
    switch (quality) {
    case Quality::Exact: return "exact";
    case Quality::Sampled: return "sampled_not_fault_site";
    default: return "unavailable";
    }
}

std::string Address(const Context& context, u64 address) {
    auto result = Hex(address);
    if (context.resolve) {
        auto resolved = context.resolve(context.user, context.process_id, address);
        if (!resolved.empty()) {
            // Keep each diagnostic a single bounded log line, even with external symbols.
            resolved.resize(std::min<std::size_t>(resolved.size(), 256));
            for (char& c : resolved) {
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
                    c = '?';
                }
            }
            result += "(" + resolved + ")";
        }
    }
    return result;
}

std::string_view StopName(WalkStop stop) {
    switch (stop) {
    case WalkStop::End: return "end";
    case WalkStop::NoRegisters: return "registers_unavailable";
    case WalkStop::NoReader: return "reader_unavailable";
    case WalkStop::InvalidFrame: return "invalid_frame";
    case WalkStop::Unreadable: return "unreadable_frame";
    case WalkStop::Limit: return "frame_limit";
    }
    return "unknown";
}

std::string_view FaultName(FaultResult result) {
    switch (result) {
    case FaultResult::NotRequested: return "not_requested";
    case FaultResult::UnsupportedBackend: return "unsupported_backend";
    case FaultResult::InexactContext: return "inexact_context";
    case FaultResult::MissingHook: return "missing_delivery_hook";
    case FaultResult::Rejected: return "delivery_rejected";
    case FaultResult::Queued: return "queued_not_yet_delivered";
    }
    return "unknown";
}

u64 LittleEndian64(std::span<const u8, 8> bytes) {
    u64 value{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value |= static_cast<u64>(bytes[i]) << (8 * i);
    }
    return value;
}
} // namespace

std::string_view ValidateMode(Mode mode, const BackendCapabilities& capabilities) {
    if (mode == Mode::Log) {
        return {};
    }
    if (!capabilities.precise_pc || !capabilities.precise_registers) {
        return "unmapped_access=fault requires precise fault-site PC and register state";
    }
    if (!capabilities.abort_before_commit) {
        return "unmapped_access=fault requires immediate memory abort before result/writeback commit";
    }
    if (!capabilities.guest_exception_delivery) {
        return "unmapped_access=fault requires guest data-abort delivery; debugger suspension is insufficient";
    }
    return {};
}

AccessScope::AccessScope(const Context& context_)
    : context{context_}, previous{current_context} {
    current_context = &context;
}

AccessScope::~AccessScope() {
    current_context = previous;
}

FaultResult RequestGuestFault(const Context& context, const Access& access) noexcept {
    if (context.mode != Mode::Fault) {
        return FaultResult::NotRequested;
    }
    if (!ValidateMode(context.mode, context.capabilities).empty()) {
        return FaultResult::UnsupportedBackend;
    }
    if (context.pc_quality != Quality::Exact || context.registers_quality != Quality::Exact) {
        return FaultResult::InexactContext;
    }
    if (!context.queue_guest_fault) {
        return FaultResult::MissingHook;
    }
    return context.queue_guest_fault(context.user, context, access) ? FaultResult::Queued
                                                                  : FaultResult::Rejected;
}

Backtrace WalkBacktrace(const Context& context) {
    Backtrace result;
    if (context.registers_quality == Quality::Unavailable) {
        result.stop = WalkStop::NoRegisters;
        return result;
    }
    if (!context.read) {
        result.stop = WalkStop::NoReader;
        return result;
    }
    u64 fp = context.fp;
    constexpr u64 MaxStackSpan = 1024 * 1024;
    while (fp != 0 && result.count < MaxFrames) {
        if ((fp & 15) != 0 || fp < context.sp || fp - context.sp > MaxStackSpan ||
            fp > std::numeric_limits<u64>::max() - 16) {
            result.stop = WalkStop::InvalidFrame;
            return result;
        }
        std::array<u8, 16> record{};
        if (!context.read(context.user, fp, record)) {
            result.stop = WalkStop::Unreadable;
            return result;
        }
        const auto bytes = std::span<const u8, 16>{record};
        const u64 next = LittleEndian64(bytes.first<8>());
        const u64 saved_lr = LittleEndian64(bytes.last<8>());
        if (saved_lr != 0) {
            result.returns[result.count++] = saved_lr;
        } else {
            return result;
        }
        if (next != 0 && next <= fp) {
            result.stop = WalkStop::InvalidFrame;
            return result;
        }
        fp = next;
    }
    result.stop = fp != 0 ? WalkStop::Limit : WalkStop::End;
    return result;
}

std::string DescribeUnmapped(const void* memory, const Access& access) {
    if (describing) {
        return "PC=unavailable LR=unavailable backtrace=unavailable reason=recursive_diagnostic";
    }
    const Context* context = current_context;
    if (!context || !memory || context->memory != memory) {
        return "PC=unavailable LR=unavailable backtrace=unavailable reason=no_matching_guest_callback";
    }
    describing = true;
    struct Reset {
        ~Reset() { describing = false; }
    } reset;
    Context sampled = *context;
    if (sampled.sample) sampled.sample(sampled.user, sampled);
    context = &sampled;
    const auto fault = RequestGuestFault(*context, access);
    std::string result = "process=" + std::to_string(context->process_id) +
                         " thread=" + (context->thread_id ? std::to_string(*context->thread_id)
                                                          : "unavailable") +
                         " core=" + std::to_string(context->core_id);
    result += " PC=" + (context->pc_quality == Quality::Unavailable ? "unavailable"
                                                                  : Address(*context, context->pc));
    result += " pc_quality=";
    result += QualityName(context->pc_quality);
    result += " LR=" + (context->registers_quality == Quality::Unavailable
                            ? "unavailable" : Address(*context, context->lr));
    result += " registers_quality=";
    result += QualityName(context->registers_quality);
    const auto backtrace = WalkBacktrace(*context);
    result += " backtrace_fp_returns_cpu_visible=[";
    for (std::size_t i = 0; i < backtrace.count; ++i) {
        if (i != 0) {
            result += ", ";
        }
        result += Address(*context, backtrace.returns[i]);
    }
    result += "] walk_stop=";
    result += StopName(backtrace.stop);
    result += " fault=";
    result += FaultName(fault);
    return result;
}

} // namespace Core::TraceFault
