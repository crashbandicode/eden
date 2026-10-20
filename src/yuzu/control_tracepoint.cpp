// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <limits>
#include <optional>

#include <QJsonArray>
#include <QRegularExpression>

#include "core/arm/debug.h"
#include "core/core.h"
#include "core/debugger/guest_tracepoint.h"
#include "core/hle/kernel/k_process.h"
#include "yuzu/control_server.h"
#include "yuzu/control_tracepoint.h"

namespace {
using Trace = Core::GuestTracepoint;

QString Hex(u64 value) {
    return QStringLiteral("0x") + QString::number(value, 16);
}

QString RegisterName(u8 index) {
    if (index < 31) return QStringLiteral("x") + QString::number(index);
    if (index == 31) return QStringLiteral("sp");
    if (index == 32) return QStringLiteral("pc");
    return QStringLiteral("pstate");
}

std::optional<u8> RegisterIndex(const QJsonValue& value) {
    if (!value.isString()) return {};
    const auto name = value.toString();
    for (u8 index = 0; index < Trace::RegisterCount; ++index) {
        if (name == RegisterName(index)) return index;
    }
    return {};
}

bool Unsigned(const QJsonValue& value, u64& result) {
    if (value.isString()) {
        const auto text = value.toString();
        static const QRegularExpression decimal{QStringLiteral("^[0-9]+$")};
        static const QRegularExpression hex{QStringLiteral("^0[xX][0-9a-fA-F]+$")};
        bool ok{};
        if (hex.match(text).hasMatch()) result = text.mid(2).toULongLong(&ok, 16);
        else if (decimal.match(text).hasMatch()) result = text.toULongLong(&ok, 10);
        return ok;
    }
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number > 9007199254740991.0 ||
        number != std::trunc(number)) return false;
    result = static_cast<u64>(number);
    return true;
}

QJsonObject Invalid() {
    return ControlServer::Error(QStringLiteral("invalid_tracepoint"),
                               QStringLiteral("Invalid tracepoint fields; inspect schema"));
}

QJsonObject Describe(const Trace::Result& result) {
    QString state;
    switch (result.state) {
    case Trace::State::Idle: state = QStringLiteral("idle"); break;
    case Trace::State::Armed: state = QStringLiteral("armed"); break;
    case Trace::State::Captured: state = QStringLiteral("captured"); break;
    case Trace::State::Expired: state = QStringLiteral("expired"); break;
    case Trace::State::Cancelled: state = QStringLiteral("cancelled"); break;
    }
    QJsonObject registers;
    QJsonArray selection;
    for (std::size_t i = 0; i < result.config.registers.size(); ++i) {
        const auto name = RegisterName(result.config.registers[i]);
        selection.append(name);
        if (i < result.values.size()) registers.insert(name, Hex(result.values[i]));
    }
    const QByteArray bytes{reinterpret_cast<const char*>(result.memory.data()),
                           static_cast<qsizetype>(result.memory.size())};
    return {{QStringLiteral("id"), QString::number(result.id)},
            {QStringLiteral("state"), state},
            {QStringLiteral("address"), Hex(result.config.address)},
            {QStringLiteral("process_id"), QString::number(result.process_id)},
            {QStringLiteral("thread_id"), QString::number(result.thread_id)},
            {QStringLiteral("selected_registers"), selection},
            {QStringLiteral("registers"), registers},
            {QStringLiteral("remaining_ms"), static_cast<int>(result.remaining_ms)},
            {QStringLiteral("max_hits"), 1},
            {QStringLiteral("automatic_resume"), true},
            {QStringLiteral("memory"), QJsonObject{
                {QStringLiteral("address"), Hex(result.memory_address)},
                {QStringLiteral("requested_size"), static_cast<int>(result.config.memory_size)},
                {QStringLiteral("hex"), QString::fromLatin1(bytes.toHex())},
                {QStringLiteral("error"), QString::fromStdString(result.memory_error)},
                {QStringLiteral("coherence"), QStringLiteral("cpu_visible_not_globally_atomic")}}}};
}
} // namespace

QJsonObject HandleGuestTracepoint(Core::System& system, const QString& method,
                                 const QJsonObject& params) {
    auto& trace = system.GetGuestTracepoint();
    if (method == QStringLiteral("tracepoint.status") || method == QStringLiteral("tracepoint.cancel")) {
        if (!params.isEmpty()) return Invalid();
        if (method == QStringLiteral("tracepoint.cancel")) trace.Cancel();
        return ControlServer::Success(Describe(trace.Status()));
    }
    if (method != QStringLiteral("tracepoint.arm")) {
        return ControlServer::Error(QStringLiteral("unknown_method"), QStringLiteral("Unknown tracepoint method"));
    }
    const QStringList allowed{QStringLiteral("address"), QStringLiteral("registers"),
                              QStringLiteral("memory_address"), QStringLiteral("memory_register"),
                              QStringLiteral("memory_offset"), QStringLiteral("memory_size"),
                              QStringLiteral("timeout_ms")};
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (!allowed.contains(it.key())) return Invalid();
    }
    Trace::Config config;
    if (!Unsigned(params.value(QStringLiteral("address")), config.address) ||
        !config.address || config.address % 4 ||
        config.address > std::numeric_limits<u64>::max() - 4) return Invalid();
    const auto selection = params.value(QStringLiteral("registers"));
    if (!selection.isArray() || selection.toArray().isEmpty() ||
        selection.toArray().size() > Trace::RegisterCount) return Invalid();
    for (const auto& value : selection.toArray()) {
        const auto reg = RegisterIndex(value);
        if (!reg) return Invalid();
        config.registers.push_back(*reg);
    }
    const auto fixed = params.value(QStringLiteral("memory_address"));
    const auto indirect = params.value(QStringLiteral("memory_register"));
    if (!fixed.isUndefined() && !indirect.isUndefined()) return Invalid();
    if (!fixed.isUndefined() && !Unsigned(fixed, config.memory_address)) return Invalid();
    if (!indirect.isUndefined()) {
        config.memory_register = RegisterIndex(indirect);
        if (!config.memory_register || *config.memory_register == 33) return Invalid();
    }
    const bool has_memory = !fixed.isUndefined() || !indirect.isUndefined();
    u64 size = has_memory ? 64 : 0;
    if (params.contains(QStringLiteral("memory_size")) &&
        !Unsigned(params.value(QStringLiteral("memory_size")), size)) return Invalid();
    if (size > Trace::MaxMemoryBytes || (has_memory ? size == 0 : size != 0)) return Invalid();
    config.memory_size = static_cast<u32>(size);
    const auto offset = params.value(QStringLiteral("memory_offset"));
    if (!offset.isUndefined()) {
        if (!has_memory || !offset.isDouble()) return Invalid();
        const double number = offset.toDouble();
        if (!std::isfinite(number) || number != std::trunc(number) ||
            number < -1048576 || number > 1048576) return Invalid();
        config.memory_offset = static_cast<s64>(number);
    }
    u64 timeout = config.timeout_ms;
    if (params.contains(QStringLiteral("timeout_ms")) &&
        !Unsigned(params.value(QStringLiteral("timeout_ms")), timeout)) return Invalid();
    if (!timeout || timeout > Trace::MaxTimeoutMs) return Invalid();
    config.timeout_ms = static_cast<u32>(timeout);

#ifndef ARCHITECTURE_x86_64
    return ControlServer::Error(QStringLiteral("tracepoint_unsupported"),
                               QStringLiteral("Tracepoints currently require x86-64 Dynarmic and an AArch64 guest"));
#else
    auto* process = system.ApplicationProcess();
    if (!system.IsPoweredOn() || system.IsShuttingDown() || system.IsPaused() || !process) {
        return ControlServer::Error(QStringLiteral("not_running"), QStringLiteral("A game must be running"));
    }
    if (!process->Is64Bit() || system.DebuggerEnabled()) {
        return ControlServer::Error(QStringLiteral("tracepoint_unsupported"),
                                   QStringLiteral("Requires an AArch64 guest with the GDB stub disabled"));
    }
    if (!process->GetMemory().IsValidVirtualAddressRange(config.address, sizeof(u32))) {
        return ControlServer::Error(QStringLiteral("tracepoint_unmapped"), QStringLiteral("Tracepoint address is unmapped"));
    }
    const auto error = trace.Arm(process->GetProcessId(), config);
    if (!error.empty()) {
        return ControlServer::Error(QString::fromStdString(error),
                                   QStringLiteral("Invalid parameters or another tracepoint is armed"));
    }
    Core::InvalidateInstructionCacheRange(process, config.address, sizeof(u32));
    return ControlServer::Success(Describe(trace.Status()));
#endif
}
