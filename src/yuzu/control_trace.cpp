// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <cmath>
#include <limits>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/debugger/guest_trace.h"
#include "core/debugger/trace_modules.h"
#include "core/debugger/trace_fault.h"
#include "core/hle/kernel/k_process.h"
#include "yuzu/control_server.h"
#include "yuzu/control_trace.h"

namespace {
using Trace = Core::GuestTrace;
using CS = ControlServer;
QString Hex(u64 value) { return QStringLiteral("0x") + QString::number(value, 16); }
QString Reg(u8 i) {
    if (i < 31) return QStringLiteral("x") + QString::number(i);
    return i == 31 ? QStringLiteral("sp") : i == 32 ? QStringLiteral("pc") : QStringLiteral("pstate");
}
bool Number(const QJsonValue& value, u64& out) {
    if (value.isString()) {
        const auto text = value.toString();
        static const QRegularExpression pattern{QStringLiteral("^(0[xX][0-9a-fA-F]+|[0-9]+)$")};
        if (!pattern.match(text).hasMatch()) return false;
        bool ok{};
        out = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
                  ? text.mid(2).toULongLong(&ok, 16) : text.toULongLong(&ok, 10);
        return ok;
    }
    if (!value.isDouble()) return false;
    const auto n = value.toDouble();
    if (!std::isfinite(n) || n < 0 || n > 9007199254740991.0 || std::trunc(n) != n) return false;
    out = static_cast<u64>(n);
    return true;
}
bool Fields(const QJsonObject& object, const QStringList& allowed) {
    for (auto i = object.begin(); i != object.end(); ++i) if (!allowed.contains(i.key())) return false;
    return true;
}
QJsonObject Error(const QString& code = QStringLiteral("invalid_trace"),
                  const QString& message = QStringLiteral("Invalid tracing fields; inspect schema")) {
    return CS::Error(code, message);
}
QJsonObject Point(const Trace::Point& point) {
    const QStringList states{QStringLiteral("pending"), QStringLiteral("armed"),
                             QStringLiteral("completed"), QStringLiteral("disarmed")};
    return {{QStringLiteral("id"), QString::number(point.id)},
            {QStringLiteral("process_id"), QString::number(point.process_id)},
            {QStringLiteral("address"), Hex(point.address)},
            {QStringLiteral("target"), QString::fromStdString(point.config.target)},
            {QStringLiteral("state"), states[static_cast<int>(point.state)]},
            {QStringLiteral("mode"), point.config.counters_only ? QStringLiteral("counters") : QStringLiteral("records")},
            {QStringLiteral("hits"), QString::number(point.hits)},
            {QStringLiteral("matched"), QString::number(point.matched)},
            {QStringLiteral("recorded"), QString::number(point.recorded)},
            {QStringLiteral("dropped"), QString::number(point.dropped)},
            {QStringLiteral("return_overflow"), QString::number(point.return_overflow)},
            {QStringLiteral("abandoned_returns"), QString::number(point.abandoned_returns)}};
}
QJsonObject Record(const Trace::Record& record) {
    QJsonObject registers;
    for (const auto& [index, value] : record.registers) registers.insert(Reg(index), Hex(value));
    QJsonArray captures;
    for (const auto& capture : record.captures) {
        const QByteArray bytes{reinterpret_cast<const char*>(capture.bytes.data()), static_cast<qsizetype>(capture.bytes.size())};
        QJsonObject item{{QStringLiteral("expression"), QString::fromStdString(capture.spec.expression)},
                         {QStringLiteral("type"), QString::fromStdString(capture.spec.type)},
                         {QStringLiteral("address"), Hex(capture.address)},
                         {QStringLiteral("hex"), QString::fromLatin1(bytes.toHex())},
                         {QStringLiteral("error"), QString::fromStdString(capture.error)}};
        if (capture.error.empty() && capture.spec.type != "bytes") {
            u64 value{};
            for (size_t i = 0; i < std::min(capture.bytes.size(), size_t{8}); ++i) value |= u64{capture.bytes[i]} << (i * 8);
            if (capture.spec.type == "c-string") {
                const auto nul = bytes.indexOf('\0');
                item.insert(QStringLiteral("value"), QString::fromUtf8(nul < 0 ? bytes : bytes.left(nul)));
                item.insert(QStringLiteral("terminated"), nul >= 0);
            } else if (capture.spec.type == "bool") item.insert(QStringLiteral("value"), value != 0);
            else if (capture.spec.type == "i32") item.insert(QStringLiteral("value"), std::bit_cast<s32>(static_cast<u32>(value)));
            else if (capture.spec.type == "f32") {
                const float f = std::bit_cast<float>(static_cast<u32>(value));
                item.insert(QStringLiteral("value"), std::isfinite(f) ? QJsonValue{f} : QJsonValue{});
                if (!std::isfinite(f)) item.insert(QStringLiteral("decode_error"), QStringLiteral("nonfinite"));
            } else item.insert(QStringLiteral("value"), QString::number(value));
        }
        captures.append(item);
    }
    QJsonArray backtrace;
    for (const auto address : record.backtrace) backtrace.append(Hex(address));
    QJsonObject locations;
    for (const auto& [address, text] : record.locations) locations.insert(Hex(address), QString::fromStdString(text));
    return {{QStringLiteral("schema"), 1}, {QStringLiteral("sequence"), QString::number(record.sequence)},
            {QStringLiteral("point_id"), QString::number(record.point_id)},
            {QStringLiteral("process_id"), QString::number(record.process_id)},
            {QStringLiteral("thread_id"), QString::number(record.thread_id)},
            {QStringLiteral("core_id"), static_cast<int>(record.core_id)},
            {QStringLiteral("frame"), QString::number(record.frame)},
            {QStringLiteral("wall_time_ns"), QString::number(record.wall_time_ns)},
            {QStringLiteral("event"), record.returning ? QStringLiteral("return") : QStringLiteral("entry")},
            {QStringLiteral("registers"), registers}, {QStringLiteral("captures"), captures},
            {QStringLiteral("backtrace"), backtrace}, {QStringLiteral("locations"), locations}};
}

bool ParsePoint(const QJsonObject& params, Trace::Config& config) {
    if (!Fields(params, {QStringLiteral("address"), QStringLiteral("target"), QStringLiteral("registers"), QStringLiteral("captures"), QStringLiteral("condition"), QStringLiteral("mode"), QStringLiteral("on_return"), QStringLiteral("backtrace"), QStringLiteral("max_hits"), QStringLiteral("rate_limit")})) return false;
    if (params.contains(QStringLiteral("address")) && !Number(params.value(QStringLiteral("address")), config.address)) return false;
    if (params.contains(QStringLiteral("target"))) {
        if (!params.value(QStringLiteral("target")).isString()) return false;
        config.target = params.value(QStringLiteral("target")).toString().toStdString();
        if (config.target.empty()) return false;
    }
    if (params.contains(QStringLiteral("registers"))) {
        if (!params.value(QStringLiteral("registers")).isArray()) return false;
        config.registers.clear();
        for (const auto value : params.value(QStringLiteral("registers")).toArray()) {
            bool found{};
            for (u8 i = 0; i < 34; ++i) {
                if (value.isString() && value.toString() == Reg(i)) { config.registers.push_back(i); found = true; break; }
            }
            if (!found) return false;
        }
    }
    if (params.contains(QStringLiteral("condition"))) {
        if (!params.value(QStringLiteral("condition")).isString()) return false;
        config.condition = params.value(QStringLiteral("condition")).toString().toStdString();
    }
    if (params.contains(QStringLiteral("mode"))) {
        if (params.value(QStringLiteral("mode")) != QJsonValue{QStringLiteral("records")} && params.value(QStringLiteral("mode")) != QJsonValue{QStringLiteral("counters")}) return false;
        config.counters_only = params.value(QStringLiteral("mode")) == QJsonValue{QStringLiteral("counters")};
    }
    for (const auto& key : {QStringLiteral("on_return"), QStringLiteral("backtrace")}) {
        if (params.contains(key) && !params.value(key).isBool()) return false;
    }
    config.on_return = params.value(QStringLiteral("on_return")).toBool();
    config.backtrace = params.value(QStringLiteral("backtrace")).toBool();
    if (params.contains(QStringLiteral("max_hits")) && !Number(params.value(QStringLiteral("max_hits")), config.max_hits)) return false;
    u64 rate = config.rate_limit;
    if (params.contains(QStringLiteral("rate_limit")) && !Number(params.value(QStringLiteral("rate_limit")), rate)) return false;
    if (!rate || rate > 10000) return false;
    config.rate_limit = static_cast<u32>(rate);
    if (params.contains(QStringLiteral("captures"))) {
        if (!params.value(QStringLiteral("captures")).isArray()) return false;
        for (const auto value : params.value(QStringLiteral("captures")).toArray()) {
            if (!value.isObject()) return false;
            const auto object = value.toObject();
            if (!Fields(object, {QStringLiteral("expression"), QStringLiteral("type"), QStringLiteral("size")}) || !object.value(QStringLiteral("expression")).isString()) return false;
            Trace::CaptureSpec spec;
            spec.expression = object.value(QStringLiteral("expression")).toString().toStdString();
            if (object.contains(QStringLiteral("type"))) {
                if (!object.value(QStringLiteral("type")).isString()) return false;
                spec.type = object.value(QStringLiteral("type")).toString().toStdString();
            }
            u64 size = 8;
            if (object.contains(QStringLiteral("size")) && !Number(object.value(QStringLiteral("size")), size)) return false;
            if (!size || size > Trace::MaxCaptureBytes) return false;
            spec.size = static_cast<u32>(size);
            config.captures.push_back(std::move(spec));
        }
    }
    return true;
}
} // namespace

ControlTrace::ControlTrace(Core::System& system_, QObject* parent) : QObject{parent}, system{system_} {
    auto* timer = new QTimer(this);
    timer->setInterval(20);
    connect(timer, &QTimer::timeout, this, [this] { Drain(); });
    timer->start();
}
ControlTrace::~ControlTrace() {
    system.GetGuestTrace().Disarm(0);
    process_ready = false;
    Flush();
}

void ControlTrace::Refresh(Kernel::KProcess* process) {
    if (!process) return;
    auto& trace = system.GetGuestTrace();
    auto& modules = system.GetTraceModules();
    trace.ResolveTargets(process->GetProcessId(), modules.Revision(),
                         [&](const auto& target) { return modules.Resolve(process->GetProcessId(), target); });
    const auto pending = trace.TakeInvalidations();
    if (pending.all) {
        for (size_t core = 0; core < Core::Hardware::NUM_CPU_CORES; ++core)
            if (auto* arm = process->GetArmInterface(core)) arm->ClearInstructionCache();
    } else {
        for (const auto address : pending.addresses) Core::InvalidateInstructionCacheRange(process, address, 4);
    }
}

bool ControlTrace::ValidateBoot() {
    auto lifetime = system.TryLockTraceProcess();
    if (!lifetime.owns_lock()) { startup_error = QStringLiteral("Trace process is shutting down"); return false; }
    auto* process = system.ApplicationProcess();
    if (system.GetGuestTrace().HasPoints() &&
        (!process || !process->Is64Bit() || system.DebuggerEnabled())) {
        system.GetGuestTrace().Disarm(0);
        startup_error = QStringLiteral("Preboot tracing requires an AArch64 guest with GDB disabled");
        return false;
    }
    startup_error.clear();
    process_ready = true;
    Refresh(process);
    return true;
}

bool ControlTrace::Flush() {
    for (size_t i = 0; i < Trace::MaxRecords / 128; ++i) Drain();
    return io_error.isEmpty();
}

void ControlTrace::Drain() {
    if (process_ready) {
        auto lifetime = system.TryLockTraceProcess();
        if (lifetime.owns_lock() && system.IsPoweredOn() && !system.IsShuttingDown())
            Refresh(system.ApplicationProcess());
    }
    if (!output.isOpen() || !io_error.isEmpty()) return;
    auto& trace = system.GetGuestTrace();
    const auto batch = trace.Read(cursor);
    QByteArray bytes;
    if (batch.lost) {
        file_lost += batch.lost;
        bytes += QJsonDocument{QJsonObject{{QStringLiteral("schema"), 1}, {QStringLiteral("event"), QStringLiteral("loss")}, {QStringLiteral("records"), QString::number(batch.lost)}}}.toJson(QJsonDocument::Compact) + '\n';
    }
    for (const auto& record : batch.records) bytes += QJsonDocument{Record(record)}.toJson(QJsonDocument::Compact) + '\n';
    if (bytes.isEmpty()) return;
    if (output.write(bytes) != bytes.size() || !output.flush()) {
        io_error = output.errorString();
        trace.Disarm(0);
        return;
    }
    cursor = batch.next_sequence;
}

QJsonObject ControlTrace::Configure(const QJsonObject& params) {
    if (!Fields(params, {QStringLiteral("run_directory"), QStringLiteral("label")}) || !params.value(QStringLiteral("run_directory")).isString() || !params.value(QStringLiteral("label")).isString()) return Error();
    if (system.IsPoweredOn() || output.isOpen()) return Error(QStringLiteral("trace_busy"), QStringLiteral("Configure once before boot"));
    const auto directory = params.value(QStringLiteral("run_directory")).toString();
    const auto label = params.value(QStringLiteral("label")).toString();
    static const QRegularExpression pattern{QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]{0,79}$")};
    if (!QDir::isAbsolutePath(directory) || !QFileInfo{directory}.isDir() || !pattern.match(label).hasMatch()) return Error();
    output.setFileName(QDir{directory}.filePath(label + QStringLiteral(".trace.jsonl")));
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return Error(QStringLiteral("trace_output"), output.errorString());
    return CS::Success(QJsonObject{{QStringLiteral("path"), output.fileName()}});
}

QJsonObject ControlTrace::Handle(const QString& method, const QJsonObject& params) {
    auto lifetime = system.TryLockTraceProcess();
    if (!lifetime.owns_lock()) return Error(QStringLiteral("trace_busy"), QStringLiteral("Main process teardown is in progress"));
    auto& trace = system.GetGuestTrace();
    auto& modules = system.GetTraceModules();
    auto* process = process_ready && system.IsPoweredOn() && !system.IsShuttingDown()
                        ? system.ApplicationProcess() : nullptr;
    const u64 pid = process ? process->GetProcessId() : 0;
    if (method == QStringLiteral("trace.unmapped_access")) {
        if (!Fields(params, {QStringLiteral("mode")}) || (params.contains(QStringLiteral("mode")) && !params.value(QStringLiteral("mode")).isString())) return Error();
        auto& state = system.GetUnmappedAccess();
#ifdef ARCHITECTURE_x86_64
        constexpr bool supported = true;
#else
        constexpr bool supported = false;
#endif
        if (params.contains(QStringLiteral("mode"))) {
            if (system.IsPoweredOn()) return Error(QStringLiteral("trace_busy"), QStringLiteral("Select unmapped access mode before boot"));
            const auto mode = params.value(QStringLiteral("mode")).toString();
            if (mode != QStringLiteral("log") && mode != QStringLiteral("fault")) return Error();
            if (mode == QStringLiteral("fault") && !supported)
                return Error(QStringLiteral("fault_unsupported"), QStringLiteral("Diagnostic stop requires the x64 Dynarmic AArch64 backend"));
            state.Configure(mode == QStringLiteral("fault") ? Core::TraceFault::Mode::Fault : Core::TraceFault::Mode::Log);
        }
        const auto status = state.Inspect();
        QJsonObject result{{QStringLiteral("mode"), state.GetMode() == Core::TraceFault::Mode::Fault ? QStringLiteral("fault") : QStringLiteral("log")},
            {QStringLiteral("run_mode"), status.mode == Core::TraceFault::Mode::Fault ? QStringLiteral("fault") : QStringLiteral("log")},
            {QStringLiteral("pc_quality"), supported ? QStringLiteral("exact") : QStringLiteral("sampled_not_fault_site")},
            {QStringLiteral("fault_supported"), supported},
            {QStringLiteral("fault_semantics"), QStringLiteral("diagnostic_stop_not_guest_exception")},
            {QStringLiteral("total"), QString::number(status.total)},
            {QStringLiteral("emitted"), QString::number(status.emitted)},
            {QStringLiteral("suppressed"), QString::number(status.suppressed)}};
        if (status.fault) {
            const auto& fault = *status.fault;
            QJsonObject registers;
            for (u8 i = 0; i < fault.registers.size(); ++i) registers.insert(Reg(i), Hex(fault.registers[i]));
            result.insert(QStringLiteral("fault"), QJsonObject{
                {QStringLiteral("address"), Hex(fault.access.address)},
                {QStringLiteral("width"), static_cast<int>(fault.access.size * 8)},
                {QStringLiteral("access"), fault.access.kind == Core::TraceFault::AccessKind::Read ? QStringLiteral("read") : QStringLiteral("write")},
                {QStringLiteral("process_id"), QString::number(fault.process_id)},
                {QStringLiteral("thread_id"), QString::number(fault.thread_id)},
                {QStringLiteral("core_id"), static_cast<int>(fault.core_id)},
                {QStringLiteral("pc"), Hex(fault.pc)}, {QStringLiteral("registers"), registers},
                {QStringLiteral("description"), QString::fromStdString(fault.description)},
                {QStringLiteral("stop_requested"), true}, {QStringLiteral("stopped"), fault.stopped}});
        }
        return CS::Success(result);
    }
    if (method == QStringLiteral("trace.configure")) return Configure(params);
    if (method == QStringLiteral("trace.list") || method == QStringLiteral("trace.counters")) {
        if (!params.isEmpty()) return Error();
        QJsonArray points;
        for (const auto& point : trace.List()) points.append(Point(point));
        return CS::Success(QJsonObject{{QStringLiteral("points"), points}, {QStringLiteral("output"), output.fileName()}, {QStringLiteral("output_error"), io_error},
                                      {QStringLiteral("boot_error"), startup_error}, {QStringLiteral("file_lost"), QString::number(file_lost)}});
    }
    if (method == QStringLiteral("trace.read")) {
        u64 after{}, limit{128};
        if (!Fields(params, {QStringLiteral("after"), QStringLiteral("limit")}) ||
            (params.contains(QStringLiteral("after")) && !Number(params.value(QStringLiteral("after")), after)) ||
            (params.contains(QStringLiteral("limit")) && !Number(params.value(QStringLiteral("limit")), limit)) || !limit || limit > 128) return Error();
        const auto batch = trace.Read(after, static_cast<size_t>(limit));
        QJsonArray records;
        for (const auto& record : batch.records) records.append(Record(record));
        return CS::Success(QJsonObject{{QStringLiteral("records"), records}, {QStringLiteral("next"), QString::number(batch.next_sequence)},
                             {QStringLiteral("lost"), QString::number(batch.lost)}, {QStringLiteral("evicted"), QString::number(batch.evicted)}});
    }
    if (method == QStringLiteral("trace.disarm")) {
        u64 id{};
        if (!Fields(params, {QStringLiteral("id")}) || (params.contains(QStringLiteral("id")) && !Number(params.value(QStringLiteral("id")), id))) return Error();
        const auto points = trace.List();
        if (!trace.Disarm(id)) return Error(QStringLiteral("trace_not_found"), QStringLiteral("Unknown point ID"));
        if (process && system.IsPoweredOn() && !system.IsShuttingDown()) {
            for (const auto& point : points) if (point.address && (!id || point.id == id)) Core::InvalidateInstructionCacheRange(process, point.address, 4);
        }
        return CS::Success();
    }
    if (method == QStringLiteral("trace.clear")) {
        if (!params.isEmpty()) return Error();
        if (system.IsPoweredOn()) return Error(QStringLiteral("trace_busy"), QStringLiteral("Stop the guest before clearing trace state"));
        if (!Flush()) return Error(QStringLiteral("trace_output"), io_error);
        trace.Clear();
        return CS::Success();
    }
    if (method == QStringLiteral("modules.list")) {
        if (!params.isEmpty()) return Error();
        QJsonArray result;
        for (const auto& module : modules.List(pid)) result.append(QJsonObject{
            {QStringLiteral("name"), QString::fromStdString(module.name)}, {QStringLiteral("build_id"), QString::fromStdString(module.build_id)},
            {QStringLiteral("base"), Hex(module.base)}, {QStringLiteral("size"), QString::number(module.size)}});
        return CS::Success(QJsonObject{{QStringLiteral("modules"), result}});
    }
    if (method == QStringLiteral("symbols.load")) {
        if (!Fields(params, {QStringLiteral("module"), QStringLiteral("elf")}) || !params.value(QStringLiteral("module")).isString() || !params.value(QStringLiteral("elf")).isString()) return Error();
        const auto error = modules.LoadSymbols(params.value(QStringLiteral("module")).toString().toStdString(), params.value(QStringLiteral("elf")).toString().toStdString());
        if (!error.empty()) return Error(QStringLiteral("symbols_invalid"), QString::fromStdString(error));
        Refresh(process);
        return CS::Success();
    }
    if (method != QStringLiteral("trace.arm")) return Error(QStringLiteral("unknown_method"), QStringLiteral("Unknown tracing method"));
#ifndef ARCHITECTURE_x86_64
    return Error(QStringLiteral("trace_unsupported"), QStringLiteral("Tracing requires AArch64 guests on x86-64 Dynarmic"));
#else
    if ((system.IsPoweredOn() && (!process_ready || system.IsShuttingDown())) ||
        system.DebuggerEnabled() || (process && !process->Is64Bit()))
        return Error(QStringLiteral("trace_unsupported"), QStringLiteral("Tracing requires an AArch64 guest with GDB disabled"));
    Trace::Config config;
    if (!ParsePoint(params, config)) return Error();
    if (!config.counters_only && !output.isOpen()) return Error(QStringLiteral("trace_output"), QStringLiteral("Use trace.configure before boot to select the run directory and label"));
    if (!io_error.isEmpty()) return Error(QStringLiteral("trace_output"), io_error);
    if (config.address && process && !process->GetMemory().IsValidVirtualAddressRange(config.address, 4)) return Error(QStringLiteral("trace_unmapped"), QStringLiteral("Absolute address is unmapped"));
    u64 id{};
    const auto error = trace.Arm(pid, config, id);
    if (!error.empty()) return Error(QString::fromStdString(error));
    Refresh(process);
    if (process && config.address) Core::InvalidateInstructionCacheRange(process, config.address, 4);
    return CS::Success(QJsonObject{{QStringLiteral("id"), QString::number(id)}});
#endif
}

bool ControlTrace::LoadConfig(const QString& path) {
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        startup_error = QStringLiteral("Cannot read bounded trace configuration: ") + path;
        return false;
    }
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        startup_error = QStringLiteral("Invalid trace JSON: ") + parse_error.errorString();
        return false;
    }
    const auto object = document.object();
    if (!Fields(object, {QStringLiteral("run_directory"), QStringLiteral("label"), QStringLiteral("symbols"), QStringLiteral("points"), QStringLiteral("unmapped_access")}) ||
        !object.value(QStringLiteral("points")).isArray() || object.value(QStringLiteral("points")).toArray().size() > static_cast<qsizetype>(Trace::MaxPoints) ||
        (object.contains(QStringLiteral("symbols")) && !object.value(QStringLiteral("symbols")).isArray())) {
        startup_error = QStringLiteral("Invalid trace config fields");
        return false;
    }
    const auto check = [&](const QJsonObject& response) {
        if (response.value(QStringLiteral("ok")).toBool()) return true;
        startup_error = QString::fromUtf8(QJsonDocument{response}.toJson(QJsonDocument::Compact));
        system.GetGuestTrace().Disarm(0);
        return false;
    };
    if (object.contains(QStringLiteral("unmapped_access")) && !check(Handle(QStringLiteral("trace.unmapped_access"), {{QStringLiteral("mode"), object.value(QStringLiteral("unmapped_access"))}}))) return false;
    if (!check(Configure({{QStringLiteral("run_directory"), object.value(QStringLiteral("run_directory"))}, {QStringLiteral("label"), object.value(QStringLiteral("label"))}}))) return false;
    for (const auto value : object.value(QStringLiteral("symbols")).toArray()) {
        if (!value.isObject() || !check(Handle(QStringLiteral("symbols.load"), value.toObject()))) {
            if (startup_error.isEmpty()) startup_error = QStringLiteral("Invalid symbols entry");
            return false;
        }
    }
    for (const auto value : object.value(QStringLiteral("points")).toArray()) {
        if (!value.isObject() || !check(Handle(QStringLiteral("trace.arm"), value.toObject()))) {
            if (startup_error.isEmpty()) startup_error = QStringLiteral("Invalid trace point");
            system.GetGuestTrace().Disarm(0);
            return false;
        }
    }
    return true;
}
