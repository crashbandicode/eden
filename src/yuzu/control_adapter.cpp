// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <QApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTimer>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/debugger/guest_tracepoint.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/perf_stats.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "qt_common/render/emu_thread.h"
#include "yuzu/control_adapter.h"
#include "yuzu/gif_recorder.h"
#include "yuzu/macro_recorder.h"
#include "yuzu/control_input.h"
#include "yuzu/control_server.h"
#include "yuzu/control_tracepoint.h"
#include "yuzu/main_window.h"
#include "yuzu/bootmanager.h"
#include "video_core/shader_notify.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2_profile.h"

namespace {
using CS = ControlServer;

bool Sensitive(const QString& key) {
    return key.contains(QStringLiteral("token"), Qt::CaseInsensitive) ||
           key.contains(QStringLiteral("password"), Qt::CaseInsensitive);
}

QJsonObject DescribeSetting(const QString& scope, const std::string& key,
                            const Settings::BasicSetting& setting) {
    const auto label = QString::fromStdString(key);
    const bool sensitive = Sensitive(label);
    return {{QStringLiteral("key"), QString{scope + QLatin1Char('.') + label}},
            {QStringLiteral("scope"), scope},
            {QStringLiteral("label"), label},
            {QStringLiteral("category"), QString::fromUtf8(Settings::TranslateCategory(setting.GetCategory()))},
            {QStringLiteral("value"), sensitive ? QJsonValue{} : QJsonValue{QString::fromStdString(setting.ToString())}},
            {QStringLiteral("default"), sensitive ? QJsonValue{} : QJsonValue{QString::fromStdString(setting.DefaultToString())}},
            {QStringLiteral("redacted"), sensitive},
            {QStringLiteral("type"), QString::fromUtf8(setting.TypeId().data(), static_cast<int>(setting.TypeId().size()))},
            {QStringLiteral("enum"), setting.IsEnum()},
            {QStringLiteral("canonical"), sensitive ? QJsonValue{} : QJsonValue{QString::fromStdString(setting.Canonicalize())}},
            {QStringLiteral("ranged"), setting.Ranged()},
            {QStringLiteral("minimum"), QString::fromStdString(setting.MinVal())},
            {QStringLiteral("maximum"), QString::fromStdString(setting.MaxVal())},
            {QStringLiteral("runtime_modifiable"), setting.RuntimeModifiable()},
            {QStringLiteral("using_global"), setting.UsingGlobal()},
            {QStringLiteral("switchable"), setting.Switchable()},
            {QStringLiteral("persistent"), setting.Save()}};
}

struct SettingRef {
    QString scope;
    std::string key;
    Settings::BasicSetting* setting{};
};

SettingRef FindSetting(const QString& key) {
    for (auto [scope, linkage] : {
             std::pair{QStringLiteral("core"), &Settings::values.linkage},
             std::pair{QStringLiteral("ui"), &UISettings::values.linkage}}) {
        const auto prefix = scope + QLatin1Char('.');
        if (!key.startsWith(prefix)) {
            continue;
        }
        const auto local_key = key.mid(prefix.size()).toStdString();
        const auto found = linkage->by_key.find(local_key);
        if (found != linkage->by_key.end()) {
            return {scope, local_key, found->second};
        }
    }
    return {};
}

// BasicSetting::LoadString intentionally falls back to defaults on malformed input.
// The control API must reject malformed/overflowing input instead of silently applying it.
bool SetValidated(Settings::BasicSetting& setting, const QJsonValue& value, QString& error) {
    QString text;
    if (setting.TypeId() == "string") {
        if (!value.isString()) {
            error = QStringLiteral("This setting requires a JSON string");
            return false;
        }
        // Eden disables RTTI. TypeId() and Ranged() identify the Setting specialization;
        // SetValue remains virtual for SwitchableSetting's effective storage.
        if (setting.Ranged()) {
            static_cast<Settings::Setting<std::string, true>&>(setting).SetValue(value.toString().toStdString());
        } else {
            static_cast<Settings::Setting<std::string>&>(setting).SetValue(value.toString().toStdString());
        }
        return true;
    }
    if (setting.TypeId() == "bool") {
        if (!value.isBool()) {
            error = QStringLiteral("This setting requires a JSON boolean");
            return false;
        }
        text = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    } else if (value.isString()) {
        text = value.toString();
    } else if (value.isDouble() && std::isfinite(value.toDouble())) {
        if ((setting.IsIntegral() || setting.IsEnum()) &&
            (std::trunc(value.toDouble()) != value.toDouble() || std::abs(value.toDouble()) > 9007199254740991.0)) {
            error = QStringLiteral("Integer values must be exact; send large integers as decimal strings");
            return false;
        }
        text = QString::number(value.toDouble(), 'g', 17);
    } else {
        error = QStringLiteral("Expected a finite number or decimal string");
        return false;
    }
    static const QRegularExpression integer{QStringLiteral("^-?[0-9]+$")};
    static const QRegularExpression floating{QStringLiteral("^-?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?$")};
    if (setting.TypeId() != "bool" &&
        !(setting.IsFloatingPoint() ? floating : integer).match(text).hasMatch()) {
        error = QStringLiteral("Invalid numeric representation; enum settings use their numeric value");
        return false;
    }
    const std::string previous = setting.ToString();
    setting.LoadString(text.toStdString());
    const auto actual = QString::fromStdString(setting.ToString());
    bool matches = actual == text;
    if (setting.IsFloatingPoint()) {
        bool requested_ok{}, actual_ok{};
        const double requested = text.toDouble(&requested_ok);
        const double result = actual.toDouble(&actual_ok);
        matches = requested_ok && actual_ok && std::isfinite(requested) && std::isfinite(result) &&
                  std::abs(requested - result) <= 1e-6 * std::max(1.0, std::abs(requested));
    } else if (setting.IsIntegral() || setting.IsEnum()) {
        // Decimal normalization without lossy JSON double conversion.
        bool requested_ok{}, actual_ok{};
        const auto requested = text.toLongLong(&requested_ok);
        const auto result = actual.toLongLong(&actual_ok);
        matches = requested_ok && actual_ok && requested == result;
    }
    if (!matches) {
        setting.LoadString(previous);
        error = QStringLiteral("Value is outside the setting's supported range");
    }
    return matches;
}
} // namespace

ControlAdapter::ControlAdapter(MainWindow& window_, ControlServer& server_)
    : QObject{&window_}, window{window_}, server{server_},
      input{std::make_unique<ControlInput>(*window.input_subsystem)} {
    telemetry_clock.start();
    auto* trace_timer = new QTimer(this);
    trace_timer->setInterval(50);
    connect(trace_timer, &QTimer::timeout, this, [] {
        QtCommon::system->GetGuestTracepoint().Poll();
    });
    trace_timer->start();
    connect(window.macro_recorder.get(), &MacroRecorder::PlaybackStarting, this,
            [this](int player) { input->ResetPlayer(player); });
    server.SetHandler([this](const QString& method, const QJsonObject& params) {
        return Handle(method, params);
    });
    connect(&window, &MainWindow::EmulationStopping, this, [this] {
        input->Reset();
        last_stats_ms = last_frame_ms = -1;
        game_fps = system_fps = 0;
        shaders_building = 0;
    });
    connect(&window, &MainWindow::statsUpdated, this,
            [this](const Core::PerfStatsResults& results, const VideoCore::ShaderNotify& shaders) {
        last_stats_ms = telemetry_clock.elapsed();
        game_fps = std::isfinite(results.average_game_fps) ? results.average_game_fps : 0;
        system_fps = std::isfinite(results.system_fps) ? results.system_fps : 0;
        shaders_building = shaders.PendingShaders();
        if (game_fps > 0) last_frame_ms = last_stats_ms;
    });
}

ControlAdapter::~ControlAdapter() {
    server.SetHandler({});
}

QJsonObject ControlAdapter::QueueGame(const QString& method, std::function<QJsonObject()> operation) {
    if (game_job_pending) {
        return CS::Error(QStringLiteral("busy"), QStringLiteral("Another game operation is still running; inspect jobs.list and active dialogs"));
    }
    game_job_pending = true;
    QPointer<ControlAdapter> self{this};
    auto result = server.Enqueue(method, [self, operation = std::move(operation)] {
        if (!self) {
            return CS::Error(QStringLiteral("destroyed"), QStringLiteral("Frontend closed before the operation began"));
        }
        struct Release {
            QPointer<ControlAdapter> adapter;
            ~Release() { if (adapter) adapter->game_job_pending = false; }
        } release{self};
        if (QApplication::activeModalWidget()) {
            return CS::Error(QStringLiteral("modal_active"), QStringLiteral("A dialog opened before the operation began"));
        }
        auto response = operation();
        // Returned status describes completion, rather than the still-active callback.
        auto status = response.value(QStringLiteral("result")).toObject();
        if (status.contains(QStringLiteral("game_operation_pending"))) {
            status.insert(QStringLiteral("game_operation_pending"), false);
            response.insert(QStringLiteral("result"), status);
        }
        return response;
    });
    if (!result.value(QStringLiteral("ok")).toBool()) game_job_pending = false;
    return result;
}

QJsonObject ControlAdapter::Handle(const QString& method, const QJsonObject& params) {
    if (method.startsWith(QStringLiteral("tracepoint."))) {
        if (method == QStringLiteral("tracepoint.arm") &&
            (game_job_pending || window.IsBootInProgress() ||
             !QtCommon::emu_thread || !QtCommon::emu_thread->IsRunning())) {
            return CS::Error(QStringLiteral("not_running"), QStringLiteral("A game must be running"));
        }
        return HandleGuestTracepoint(*QtCommon::system, method, params);
    }
    if (method.startsWith(QStringLiteral("macro."))) {
        return window.macro_recorder->Handle(method, params);
    }
    if (method.startsWith(QStringLiteral("input."))) {
        if (window.macro_recorder->OwnsInput() &&
            method != QStringLiteral("input.schema") && method != QStringLiteral("input.state")) {
            return CS::Error(QStringLiteral("input_busy"),
                             QStringLiteral("Macro playback owns input; use macro.stop first"));
        }
        return input->Handle(method, params);
    }
    if (method.startsWith(QStringLiteral("recording."))) {
        if (method == QStringLiteral("recording.start")) {
            const auto integer = [&](const QString& key, int fallback) {
                const auto value = params.value(key);
                if (value.isUndefined()) return fallback;
                if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
                    value.toDouble() != std::trunc(value.toDouble()) ||
                    value.toDouble() < 0 || value.toDouble() > 120000) return -1;
                return value.toInt(-1);
            };
            QString error;
            if (!window.gif_recorder->Start(params.value(QStringLiteral("path")).toString(),
                    integer(QStringLiteral("fps"), 10), integer(QStringLiteral("width"), 640),
                    integer(QStringLiteral("duration_ms"), 30000), error)) {
                return CS::Error(QStringLiteral("recording_failed"), error);
            }
        } else if (method == QStringLiteral("recording.stop")) {
            window.gif_recorder->Stop();
        } else if (method != QStringLiteral("recording.status")) {
            return CS::Error(QStringLiteral("unknown_method"), QStringLiteral("Unknown recording method"));
        }
        return CS::Success(window.gif_recorder->Status());
    }
    if (method == QStringLiteral("status")) {
        const bool powered = QtCommon::system->IsPoweredOn();
        const auto* process = powered ? QtCommon::system->ApplicationProcess() : nullptr;
        const bool switch2 = process && process->IsSwitch2();
        const auto& memory_layout = Settings::values.memory_layout_mode;
        using Split = VideoCommon::GpuSplit;
        const auto split_state = Split::Get().GetStatus();
        const char split_mode = Split::RequestedMode();
        const QJsonObject split{
            {QStringLiteral("requested_mode"), split_mode >= '0' && split_mode <= '2' ? split_mode - '0' : -1},
            {QStringLiteral("state"), QString::fromLatin1(Split::StatusName(split_state))},
            {QStringLiteral("active"), split_state == Split::ExecutionState::ActiveBackMacros || split_state == Split::ExecutionState::ActiveFrontMacros},
            {QStringLiteral("front_macros"), split_state == Split::ExecutionState::ActiveFrontMacros},
        };
        const QJsonObject emulation{
            {QStringLiteral("generation"), process ? (switch2 ? QStringLiteral("switch2") : QStringLiteral("switch")) : QStringLiteral("none")},
            {QStringLiteral("compiled_cpu_cores"), static_cast<int>(Core::Hardware::NUM_CPU_CORES)},
            {QStringLiteral("multicore"), Settings::values.use_multi_core.GetValue()},
            {QStringLiteral("memory_layout"), QString::fromStdString(memory_layout.Canonicalize())},
            {QStringLiteral("memory_layout_value"), static_cast<int>(memory_layout.GetValue())},
            {QStringLiteral("global_memory_layout_value"), static_cast<int>(memory_layout.GetValue(true))},
            {QStringLiteral("memory_layout_automatic"), powered && QtCommon::system->WasMemoryLayoutAutoSelected()},
            {QStringLiteral("generation_forces_auto_stub"), switch2},
            {QStringLiteral("gpu_split"), split},
            {QStringLiteral("gpu_profile_enabled"), VideoCommon::Nx2Profile::Enabled()},
        };
        return CS::Success(QJsonObject{
            {QStringLiteral("ready"), true},
            {QStringLiteral("emulation"), emulation},
            {QStringLiteral("game_operation_pending"), game_job_pending},
            {QStringLiteral("powered_on"), powered},
            {QStringLiteral("running"), QtCommon::emu_thread && QtCommon::emu_thread->IsRunning()},
            // Power is cleared before teardown joins. Keep reporting shutdown
            // until the emulation thread has finished and the frontend retires it.
            {QStringLiteral("shutting_down"),
             QtCommon::emu_thread && QtCommon::system->IsShuttingDown()},
            {QStringLiteral("loading_complete"), powered && window.render_window->IsLoadingComplete()},
            {QStringLiteral("game_fps"), game_fps},
            {QStringLiteral("system_fps"), system_fps},
            {QStringLiteral("shaders_building"), shaders_building},
            {QStringLiteral("stats_age_ms"), last_stats_ms < 0 ? -1 : telemetry_clock.elapsed() - last_stats_ms},
            {QStringLiteral("last_frame_age_ms"), last_frame_ms < 0 ? -1 : telemetry_clock.elapsed() - last_frame_ms},
            {QStringLiteral("game_path"), window.current_game_path},
            {QStringLiteral("title"), window.windowTitle()},
            {QStringLiteral("commit"), QString::fromUtf8(Common::g_scm_rev)},
            {QStringLiteral("modal"), QApplication::activeModalWidget() != nullptr},
            {QStringLiteral("program_id"), powered ? QString::number(QtCommon::system->GetApplicationProcessProgramID(), 16) : QString{}}});
    }
    if (method == QStringLiteral("settings.list")) {
        QJsonArray settings;
        for (const auto& [key, setting] : Settings::values.linkage.by_key) {
            settings.append(DescribeSetting(QStringLiteral("core"), key, *setting));
        }
        for (const auto& [key, setting] : UISettings::values.linkage.by_key) {
            settings.append(DescribeSetting(QStringLiteral("ui"), key, *setting));
        }
        return CS::Success(QJsonObject{{QStringLiteral("settings"), settings},
                                      {QStringLiteral("complex_options"), QStringLiteral("Discover ui.tree after opening the relevant configuration dialog")}});
    }
    if (method == QStringLiteral("settings.get") || method == QStringLiteral("settings.set")) {
        const auto ref = FindSetting(params.value(QStringLiteral("key")).toString());
        if (!ref.setting) {
            return CS::Error(QStringLiteral("unknown_setting"), QStringLiteral("Use a key from settings.list, including core. or ui. prefix"));
        }
        if (method == QStringLiteral("settings.set")) {
            if (QApplication::activeModalWidget()) {
                return CS::Error(QStringLiteral("modal_active"), QStringLiteral("Close or apply the open dialog before changing settings directly"));
            }
            if (QtCommon::system->IsPoweredOn() && !ref.setting->RuntimeModifiable()) {
                return CS::Error(QStringLiteral("not_runtime_modifiable"), QStringLiteral("Stop emulation before changing this setting"));
            }
            if (!params.contains(QStringLiteral("value"))) {
                return CS::Error(QStringLiteral("invalid_params"), QStringLiteral("value is required"));
            }
            QString error;
            if (!SetValidated(*ref.setting, params.value(QStringLiteral("value")), error)) {
                return CS::Error(QStringLiteral("invalid_value"), error);
            }
            QtCommon::system->ApplySettings();
            window.UpdateStatusButtons();
            if (ref.key == "log_filter") {
                Common::Log::Filter filter;
                filter.ParseFilterString(Settings::values.log_filter.GetValue());
                Common::Log::SetGlobalFilter(filter);
            }
            LOG_INFO(Frontend, "Control setting changed: {}.{}", ref.scope.toStdString(), ref.key);
        }
        auto result = DescribeSetting(ref.scope, ref.key, *ref.setting);
        if (ref.scope == QStringLiteral("ui")) {
            result.insert(QStringLiteral("apply_note"), QStringLiteral("UI preferences are stored; use UI controls for immediate widget effects, or restart after settings.save"));
        }
        return CS::Success(result);
    }
    if (method == QStringLiteral("settings.save")) {
        if (QApplication::activeModalWidget()) {
            return CS::Error(QStringLiteral("modal_active"), QStringLiteral("Close or apply the open configuration dialog first"));
        }
        window.config->SaveAllValues();
        return CS::Success();
    }
    if (method == QStringLiteral("log.filter")) {
        const auto value = params.value(QStringLiteral("filter"));
        if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 4096) {
            return CS::Error(QStringLiteral("invalid_params"), QStringLiteral("filter must be a nonempty string of at most 4096 characters"));
        }
        Common::Log::Filter filter;
        filter.ParseFilterString(value.toString().toStdString());
        Common::Log::SetGlobalFilter(filter);
        Common::Log::SetColorConsoleBackendEnabled(true);
        return CS::Success(QJsonObject{{QStringLiteral("filter"), value}});
    }
    if (method.startsWith(QStringLiteral("game."))) {
        if (game_job_pending || window.IsBootInProgress()) {
            return CS::Error(QStringLiteral("busy"), QStringLiteral("Another game operation is still running; inspect jobs.list and active dialogs"));
        }
        if (QApplication::activeModalWidget()) {
            return CS::Error(QStringLiteral("modal_active"), QStringLiteral("Handle the active dialog through ui commands first"));
        }
        if (method == QStringLiteral("game.load")) {
            const auto path = params.value(QStringLiteral("path")).toString();
            if (path.isEmpty() || !QFileInfo{path}.isFile()) {
                return CS::Error(QStringLiteral("invalid_path"), QStringLiteral("path must name an existing game file"));
            }
            if (QtCommon::system->IsPoweredOn()) {
                return CS::Error(QStringLiteral("game_loaded"), QStringLiteral("Stop the current game before loading another"));
            }
            return QueueGame(method, [this, path] {
                window.BootGame(path, window.ApplicationAppletParameters());
                if (!QtCommon::system->IsPoweredOn()) {
                    return CS::Error(QStringLiteral("load_failed"), QStringLiteral("Game did not start; inspect dialogs and logs"));
                }
                return Handle(QStringLiteral("status"), {});
            });
        }
        if (method == QStringLiteral("game.pause") || method == QStringLiteral("game.resume") ||
            method == QStringLiteral("game.stop")) {
            if (!QtCommon::emu_thread || !QtCommon::system->IsPoweredOn()) {
                return CS::Error(QStringLiteral("no_game"), QStringLiteral("No game is loaded"));
            }
            return QueueGame(method, [this, method] {
                if (!QtCommon::emu_thread || !QtCommon::system->IsPoweredOn()) {
                    return CS::Error(QStringLiteral("no_game"), QStringLiteral("Game stopped before operation began"));
                }
                if (method == QStringLiteral("game.pause")) {
                    if (QtCommon::emu_thread->IsRunning()) window.OnPauseGame();
                } else if (method == QStringLiteral("game.resume")) {
                    if (!QtCommon::emu_thread->IsRunning()) window.OnStartGame();
                } else {
                    input->Reset();
                    window.OnStopGame();
                }
                return Handle(QStringLiteral("status"), {});
            });
        }
    }
    return CS::Error(QStringLiteral("unknown_method"), QStringLiteral("Unknown emulator control method"));
}
