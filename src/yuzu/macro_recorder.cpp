// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/macro_recorder.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <tuple>
#include <utility>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QSaveFile>
#include <QShortcut>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings_input.h"
#include "core/core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/tas_input.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "qt_common/qt_common.h"
#include "qt_common/render/emu_thread.h"
#include "yuzu/bootmanager.h"
#include "yuzu/control_server.h"

namespace {

constexpr int kTickMs = 16;
constexpr int kCountdownMaxMs = 120000;
// Bound capture and JSON-save memory even when noisy motion changes every frame.
constexpr std::size_t kRecordingEventLimit = 60000;
const QString kStopKeyboard = QStringLiteral("Ctrl+Shift+F12");

static_assert(EdenMacro::ButtonCount == static_cast<int>(Settings::NativeButton::NumButtons));
static_assert(static_cast<int>(Settings::NativeAnalog::NumAnalogs) == 2);
static_assert(static_cast<int>(Settings::NativeTrigger::NumTriggers) == 2);
static_assert(static_cast<int>(Settings::NativeMotion::NumMotions) == 2);

QJsonObject Ok(const QJsonValue& result) {
    return ControlServer::Success(result);
}

QJsonObject Fail(const QString& code, const QString& message) {
    return ControlServer::Error(code, message);
}

bool OnlyKeys(const QJsonObject& params, std::initializer_list<QString> allowed, QString& error) {
    for (auto it = params.begin(); it != params.end(); ++it) {
        bool known = false;
        for (const QString& key : allowed) {
            if (it.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            error = QStringLiteral("Unknown parameter \"%1\".").arg(it.key());
            return false;
        }
    }
    return true;
}

bool TakeString(const QJsonObject& params, const QString& key, bool required, QString& out,
                QString& error) {
    if (!params.contains(key)) {
        if (required) {
            error = QStringLiteral("%1 is required.").arg(key);
            return false;
        }
        return true;
    }
    if (!params.value(key).isString()) {
        error = QStringLiteral("%1 must be a string.").arg(key);
        return false;
    }
    out = params.value(key).toString();
    return true;
}

bool TakeInt(const QJsonObject& params, const QString& key, int low, int high, bool required,
             int fallback, int& out, QString& error) {
    if (!params.contains(key)) {
        if (required) {
            error = QStringLiteral("%1 is required.").arg(key);
            return false;
        }
        out = fallback;
        return true;
    }
    const QJsonValue value = params.value(key);
    if (!value.isDouble()) {
        error = QStringLiteral("%1 must be an integer.").arg(key);
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::trunc(number) != number) {
        error = QStringLiteral("%1 must be an integer.").arg(key);
        return false;
    }
    if (number < low || number > high) {
        error = QStringLiteral("%1 is out of range.").arg(key);
        return false;
    }
    out = static_cast<int>(number);
    return true;
}

int CodePoints(const QString& text) {
    int count = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar unit = text.at(i);
        char32_t code_point = unit.unicode();
        if (unit.isHighSurrogate()) {
            if (i + 1 >= text.size() || !text.at(i + 1).isLowSurrogate()) {
                return -1;
            }
            code_point = QChar::surrogateToUcs4(unit, text.at(i + 1));
            ++i;
        } else if (unit.isLowSurrogate()) {
            return -1;
        }
        if (QChar::category(code_point) == QChar::Other_Control) {
            return -1;
        }
        ++count;
    }
    return count;
}

bool CodecText(const QString& text, int max_chars, QString& error) {
    const int count = CodePoints(text);
    if (count < 0) {
        error = QStringLiteral("The text contains control characters or invalid UTF-16.");
        return false;
    }
    if (count > max_chars) {
        error = QStringLiteral("The text is longer than %1 characters.").arg(max_chars);
        return false;
    }
    return true;
}

bool CodecName(const QString& name, QString& error) {
    if (name.isEmpty()) {
        error = QStringLiteral("The name must not be empty.");
        return false;
    }
    if (name != name.trimmed()) {
        error = QStringLiteral("The name must not start or end with whitespace.");
        return false;
    }
    return CodecText(name, EdenMacro::MaxNameChars, error);
}

bool FileName(const QString& name, QString& error) {
    if (!CodecName(name, error)) {
        return false;
    }
    for (const QChar unit : name) {
        const char16_t code = unit.unicode();
        if (code == '/' || code == '\\' || code == ':' || code == '*' || code == '?' ||
            code == '"' || code == '<' || code == '>' || code == '|') {
            error = QStringLiteral(
                "The macro name contains a character that cannot be used in a file name.");
            return false;
        }
    }
    const QString stem = name.section(QLatin1Char('.'), 0, 0);
    static const QStringList reserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    if (name == QStringLiteral(".") || name == QStringLiteral("..") ||
        reserved.contains(stem, Qt::CaseInsensitive)) {
        error = QStringLiteral("The macro name is reserved by the file system.");
        return false;
    }
    return true;
}

float Clamp(float value, float low, float high) {
    if (!std::isfinite(value)) {
        return 0.F;
    }
    return std::min(high, std::max(low, value));
}

int ButtonBit(const QString& token) {
    static const QHash<QString, int> bits = {
        {QStringLiteral("a"), 0},          {QStringLiteral("b"), 1},
        {QStringLiteral("x"), 2},          {QStringLiteral("y"), 3},
        {QStringLiteral("lstick"), 4},     {QStringLiteral("left_stick"), 4},
        {QStringLiteral("l_stick"), 4},    {QStringLiteral("rstick"), 5},
        {QStringLiteral("right_stick"), 5},{QStringLiteral("r_stick"), 5},
        {QStringLiteral("l"), 6},          {QStringLiteral("r"), 7},
        {QStringLiteral("zl"), 8},         {QStringLiteral("zr"), 9},
        {QStringLiteral("plus"), 10},      {QStringLiteral("minus"), 11},
        {QStringLiteral("left"), 12},      {QStringLiteral("dpad_left"), 12},
        {QStringLiteral("up"), 13},        {QStringLiteral("dpad_up"), 13},
        {QStringLiteral("right"), 14},     {QStringLiteral("dpad_right"), 14},
        {QStringLiteral("down"), 15},      {QStringLiteral("dpad_down"), 15},
        {QStringLiteral("sl_left"), 16},   {QStringLiteral("sl"), 16},
        {QStringLiteral("slleft"), 16},    {QStringLiteral("sr_left"), 17},
        {QStringLiteral("sr"), 17},        {QStringLiteral("srleft"), 17},
        {QStringLiteral("home"), 18},      {QStringLiteral("capture"), 19},
        {QStringLiteral("screenshot"), 19},{QStringLiteral("sl_right"), 20},
        {QStringLiteral("slright"), 20},   {QStringLiteral("sr_right"), 21},
        {QStringLiteral("srright"), 21},
    };
    const auto it = bits.constFind(token.trimmed().toLower());
    if (it == bits.constEnd()) {
        return -1;
    }
    return it.value();
}

Core::HID::EmulatedController* ControllerAt(int index) {
    if (!QtCommon::system || index < 0 || index > EdenMacro::MaxPlayer) {
        return nullptr;
    }
    return QtCommon::system->HIDCore().GetEmulatedControllerByIndex(static_cast<std::size_t>(index));
}

QString CurrentGameId() {
    if (!QtCommon::system || !QtCommon::system->IsPoweredOn()) {
        return {};
    }
    const u64 id = QtCommon::system->GetApplicationProcessProgramID();
    if (id == 0) {
        return {};
    }
    return QString::number(static_cast<qulonglong>(id), 16)
        .rightJustified(EdenMacro::GameIdChars, QLatin1Char('0'))
        .toUpper();
}

bool GateFor(const GRenderWindow& render, int index, QString& error) {
    if (QtCommon::system && QtCommon::system->IsShuttingDown()) {
        error = QStringLiteral("The game is shutting down.");
        return false;
    }
    if (!render.IsLoadingComplete()) {
        error = QStringLiteral("The game is still loading.");
        return false;
    }
    if (!QtCommon::system || !QtCommon::system->IsPoweredOn()) {
        error = QStringLiteral("The game is not running.");
        return false;
    }
    if (!QtCommon::emu_thread || !QtCommon::emu_thread->IsRunning()) {
        error = QStringLiteral("Emulation is paused.");
        return false;
    }
    if (QApplication::activeModalWidget() != nullptr) {
        error = QStringLiteral("A dialog is open.");
        return false;
    }
    if (auto* controller = ControllerAt(index); controller && controller->IsConfiguring()) {
        error = QStringLiteral("Controller input is being configured.");
        return false;
    }
    error.clear();
    return true;
}

bool DevicesReady(int index, QString& error) {
    auto* controller = ControllerAt(index);
    if (!controller || !controller->InputDevicesLoaded()) {
        error = QStringLiteral("Controller input is not loaded.");
        return false;
    }
    if (!controller->IsConnected()) {
        error = QStringLiteral("The selected controller is not connected.");
        return false;
    }
    return true;
}

QShortcut* MakeShortcut(const QKeySequence& sequence, QWidget& parent) {
    auto* shortcut = new QShortcut(sequence, &parent);
    shortcut->setContext(Qt::ApplicationShortcut);
    shortcut->setAutoRepeat(false);
    return shortcut;
}

bool KeyboardAvailable(const QKeySequence& sequence, QWidget& owner,
                       const QShortcut* ignored, QString& error) {
    if (sequence.isEmpty()) return true;
    for (const auto* shortcut : owner.findChildren<QShortcut*>()) {
        if (shortcut != ignored && shortcut->key() == sequence) {
            error = QStringLiteral("Keyboard shortcut %1 is already assigned.")
                        .arg(sequence.toString(QKeySequence::PortableText));
            return false;
        }
    }
    return true;
}

} // namespace

MacroRecorder::MacroRecorder(GRenderWindow& render_window, InputCommon::InputSubsystem& input_subsystem,
                             QWidget& owner_widget)
    : render{render_window}, input{input_subsystem}, owner{owner_widget} {
    const std::filesystem::path root =
        Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "input_macros";
    directory = QString::fromStdString(Common::FS::PathToUTF8String(root));
    if (!QDir{}.mkpath(directory)) {
        directory_error = QStringLiteral("Cannot create the input_macros directory.");
        LOG_ERROR(Frontend, "Cannot create input macro directory {}", directory.toStdString());
    }

    indicator = new QLabel(&render);
    indicator->setObjectName(QStringLiteral("MacroCue"));
    indicator->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
                              Qt::WindowTransparentForInput);
    indicator->setAttribute(Qt::WA_ShowWithoutActivating, true);
    indicator->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    indicator->setFocusPolicy(Qt::NoFocus);
    indicator->setAlignment(Qt::AlignCenter);
    indicator->hide();

    const QKeySequence stop_sequence =
        QKeySequence::fromString(kStopKeyboard, QKeySequence::PortableText);
    stop_shortcut = MakeShortcut(stop_sequence, owner);
    QObject::connect(stop_shortcut, &QShortcut::activated, this, [this] { Stop(); });

    LoadShortcutFile();
    ReloadBindings();

    QObject::connect(&timer, &QTimer::timeout, this, [this] { Tick(); });
    timer.setInterval(kTickMs);
    timer.start();
    render.SetMacroFrameCallback([this] { AdvanceFrame(); });
}

MacroRecorder::~MacroRecorder() {
    shutting_down = true;
    render.SetMacroFrameCallback({});
    timer.stop();
    bool flush = false;
    {
        std::scoped_lock lock{mutex};
        flush = save_pending || state == QStringLiteral("recording");
        can_advance.store(false, std::memory_order_release);
        if (!flush) {
            if (injected) {
                Neutral();
            }
            ReleaseExclusive();
        }
    }
    if (flush) {
        Finish();
    }
    if (marker_shortcut) {
        delete marker_shortcut.data();
    }
    if (stop_shortcut) {
        delete stop_shortcut.data();
    }
    for (Binding& binding : bindings) {
        if (binding.keyboard) {
            delete binding.keyboard.data();
        }
    }
    bindings.clear();
    if (indicator) {
        delete indicator.data();
    }
}

QJsonObject MacroRecorder::Handle(const QString& method, const QJsonObject& params) {
    if (entering) {
        return Fail(QStringLiteral("busy"),
                    QStringLiteral("A macro command is already being started."));
    }
    command_code = QStringLiteral("invalid_params");
    QString error;
    if (method == QStringLiteral("macro.list")) {
        if (!params.isEmpty()) {
            return Fail(command_code, QStringLiteral("macro.list takes no parameters."));
        }
        return Ok(List());
    }
    if (method == QStringLiteral("macro.inspect")) {
        if (!OnlyKeys(params, {QStringLiteral("path"), QStringLiteral("name")}, error)) {
            return Fail(command_code, error);
        }
        EdenMacro::Document doc;
        QString file;
        if (!ReadDocument(params, doc, file, error)) {
            return Fail(command_code.isEmpty() ? QStringLiteral("invalid_params") : command_code,
                        error);
        }
        QJsonObject object = EdenMacro::ToJson(doc);
        object.insert(QStringLiteral("path"), file);
        object.insert(QStringLiteral("keyboard_shortcut"), doc.keyboard_shortcut);
        object.insert(QStringLiteral("gamepad_shortcut"), doc.gamepad_shortcut);
        return Ok(object);
    }
    if (method == QStringLiteral("macro.record")) {
        if (!StartRecord(params, false, error)) {
            return Fail(command_code.isEmpty() ? QStringLiteral("invalid_params") : command_code,
                        error);
        }
        NoteUi();
        return Ok(Status());
    }
    if (method == QStringLiteral("macro.play")) {
        if (!Play(params, error)) {
            return Fail(command_code.isEmpty() ? QStringLiteral("invalid_params") : command_code,
                        error);
        }
        NoteUi();
        return Ok(Status());
    }
    if (method == QStringLiteral("macro.record_from")) {
        if (!StartRecord(params, true, error)) {
            return Fail(command_code.isEmpty() ? QStringLiteral("invalid_params") : command_code,
                        error);
        }
        NoteUi();
        return Ok(Status());
    }
    if (method == QStringLiteral("macro.stop")) {
        if (!params.isEmpty()) {
            return Fail(command_code, QStringLiteral("macro.stop takes no parameters."));
        }
        Stop();
        return Ok(Status());
    }
    if (method == QStringLiteral("macro.marker")) {
        if (!OnlyKeys(params, {QStringLiteral("name")}, error)) {
            return Fail(command_code, error);
        }
        QString requested;
        if (!TakeString(params, QStringLiteral("name"), false, requested, error)) {
            return Fail(command_code, error);
        }
        {
            std::scoped_lock lock{mutex};
            if (state != QStringLiteral("recording")) {
                return Fail(QStringLiteral("not_recording"),
                            QStringLiteral("Markers can be added while recording."));
            }
        }
        if (!AddMarker(requested, error)) {
            return Fail(QStringLiteral("invalid_params"), error);
        }
        QJsonObject status = Status();
        {
            std::scoped_lock lock{mutex};
            if (!document.markers.empty()) {
                const EdenMacro::Marker& marker = document.markers.back();
                status.insert(QStringLiteral("marker"),
                              QJsonObject{{QStringLiteral("name"), marker.name},
                                          {QStringLiteral("frame"),
                                           static_cast<qint64>(marker.frame)}});
            }
        }
        NoteUi();
        return Ok(status);
    }
    if (method == QStringLiteral("macro.bind")) {
        if (!Bind(params, error)) {
            return Fail(command_code.isEmpty() ? QStringLiteral("invalid_params") : command_code,
                        error);
        }
        EdenMacro::Document doc;
        QString file;
        if (!ReadDocument(params, doc, file, error)) {
            return Fail(command_code.isEmpty() ? QStringLiteral("io_error") : command_code, error);
        }
        QJsonObject object = EdenMacro::ToJson(doc);
        object.insert(QStringLiteral("path"), file);
        object.insert(QStringLiteral("keyboard_shortcut"), doc.keyboard_shortcut);
        object.insert(QStringLiteral("gamepad_shortcut"), doc.gamepad_shortcut);
        NoteUi();
        return Ok(object);
    }
    if (method == QStringLiteral("macro.shortcuts")) {
        if (!SetShortcuts(params, error)) {
            return Fail(command_code.isEmpty() ? QStringLiteral("invalid_params") : command_code,
                        error);
        }
        NoteUi();
        return Ok(QJsonObject{{QStringLiteral("marker_keyboard"), marker_keyboard},
                              {QStringLiteral("marker_gamepad"), marker_gamepad},
                              {QStringLiteral("stop_keyboard"), kStopKeyboard}});
    }
    if (method == QStringLiteral("macro.status")) {
        if (!params.isEmpty()) {
            return Fail(command_code, QStringLiteral("macro.status takes no parameters."));
        }
        return Ok(Status());
    }
    return Fail(QStringLiteral("unknown_method"), QStringLiteral("Unknown macro method."));
}

QJsonObject MacroRecorder::Status() const {
    std::scoped_lock lock{mutex};
    QString ignored;
    const bool live = state == QStringLiteral("countdown") || state == QStringLiteral("recording") ||
                      state == QStringLiteral("playing") || state == QStringLiteral("branch_playing");
    const bool paused = live && !GateFor(render, player, ignored);
    return QJsonObject{
        {QStringLiteral("state"), state},
        {QStringLiteral("cue"), Cue()},
        {QStringLiteral("name"), document.name},
        {QStringLiteral("path"), path},
        {QStringLiteral("player"), player},
        {QStringLiteral("frame"), static_cast<qint64>(frame)},
        {QStringLiteral("duration_frames"), static_cast<qint64>(document.duration_frames)},
        {QStringLiteral("countdown_remaining"), static_cast<qint64>(CountdownRemaining())},
        {QStringLiteral("paused"), paused},
        {QStringLiteral("error"), failure},
    };
}

bool MacroRecorder::IsActive() const {
    std::scoped_lock lock{mutex};
    return state == QStringLiteral("countdown") || state == QStringLiteral("recording") ||
           state == QStringLiteral("playing") || state == QStringLiteral("branch_playing");
}

bool MacroRecorder::OwnsInput() const {
    std::scoped_lock lock{mutex};
    return state == QStringLiteral("playing") || state == QStringLiteral("branch_playing");
}

void MacroRecorder::Stop() {
    bool flush = false;
    {
        std::scoped_lock lock{mutex};
        flush = save_pending || state == QStringLiteral("recording");
    }
    if (flush) {
        Finish();
    } else {
        Cancel();
    }
    NoteUi();
}

void MacroRecorder::AdvanceFrame() {
    if (!can_advance.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (!can_advance.load(std::memory_order_relaxed)) {
        return;
    }
    if (state == QStringLiteral("recording")) {
        RecordOne();
    } else if (state == QStringLiteral("playing") || state == QStringLiteral("branch_playing")) {
        PlayOne();
    }
}

bool MacroRecorder::Ready(QString& error) const {
    return GateFor(render, player, error);
}

QString MacroRecorder::Resolve(const QJsonObject& params, QString& error) const {
    command_code = QStringLiteral("invalid_params");
    const bool has_path = params.contains(QStringLiteral("path"));
    const bool has_name = params.contains(QStringLiteral("name"));
    if (has_path == has_name) {
        error = QStringLiteral("Provide either path or name.");
        return {};
    }
    QString requested;
    if (has_path) {
        if (!params.value(QStringLiteral("path")).isString()) {
            error = QStringLiteral("path must be a string.");
            return {};
        }
        requested = params.value(QStringLiteral("path")).toString();
        if (!QDir::isAbsolutePath(requested)) {
            error = QStringLiteral("path must be absolute.");
            return {};
        }
    } else {
        if (!params.value(QStringLiteral("name")).isString()) {
            error = QStringLiteral("name must be a string.");
            return {};
        }
        const QString name = params.value(QStringLiteral("name")).toString();
        if (!CodecName(name, error)) {
            return {};
        }
        if (!directory_error.isEmpty()) {
            command_code = QStringLiteral("io_error");
            error = directory_error;
            return {};
        }
        for (const QString& candidate : LibraryFiles()) {
            EdenMacro::Document doc;
            QString ignored;
            if (EdenMacro::Load(candidate, doc, ignored) && doc.name == name) {
                if (!requested.isEmpty()) {
                    error = QStringLiteral("More than one macro has that name; use its path.");
                    return {};
                }
                requested = candidate;
            }
        }
        if (requested.isEmpty()) {
            command_code = QStringLiteral("not_found");
            error = QStringLiteral("The named macro was not found.");
            return {};
        }
    }
    const QFileInfo info{requested};
    if (!info.exists()) {
        command_code = QStringLiteral("not_found");
        error = QStringLiteral("The macro file was not found.");
        return {};
    }
    if (info.isSymLink()) {
        command_code = QStringLiteral("invalid_file");
        error = QStringLiteral("The macro path is a symbolic link.");
        return {};
    }
    if (!info.isFile()) {
        command_code = QStringLiteral("invalid_file");
        error = QStringLiteral("The macro path is not a file.");
        return {};
    }
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty()) {
        command_code = QStringLiteral("io_error");
        error = QStringLiteral("The macro path could not be resolved.");
        return {};
    }
    command_code.clear();
    return canonical;
}

QString MacroRecorder::OutputPath(const QString& name, const QString& requested,
                                  QString& error) const {
    command_code = QStringLiteral("invalid_params");
    for (const QString& existing : LibraryFiles()) {
        EdenMacro::Document doc;
        QString ignored;
        if (EdenMacro::Load(existing, doc, ignored) && doc.name == name) {
            command_code = QStringLiteral("already_exists");
            error = QStringLiteral("A macro with that name already exists.");
            return {};
        }
    }
    const QString settings = directory + QStringLiteral("/marker_shortcuts.json");
    if (!requested.isEmpty()) {
        if (!QDir::isAbsolutePath(requested)) {
            error = QStringLiteral("The output path must be absolute.");
            return {};
        }
        const QFileInfo info{requested};
        if (info.absoluteFilePath() == QFileInfo{settings}.absoluteFilePath()) {
            error = QStringLiteral("That path is the marker shortcut settings file.");
            return {};
        }
        if (info.exists()) {
            if (info.isSymLink() || !info.isFile()) {
                error = QStringLiteral("The output path is a symbolic link or not a regular file.");
                return {};
            }
            command_code = QStringLiteral("already_exists");
            error = QStringLiteral("A file already exists at the output path.");
            return {};
        }
        const QFileInfo parent{info.absolutePath()};
        if (!parent.exists() || !parent.isDir()) {
            command_code = QStringLiteral("io_error");
            error = QStringLiteral("The output directory does not exist.");
            return {};
        }
        command_code.clear();
        return info.absoluteFilePath();
    }
    if (!directory_error.isEmpty()) {
        command_code = QStringLiteral("io_error");
        error = directory_error;
        return {};
    }
    QString file = directory + QLatin1Char('/') + name;
    if (!file.endsWith(QStringLiteral(".edenmacro.json"))) {
        file += QStringLiteral(".edenmacro.json");
    }
    const QFileInfo info{file};
    if (info.exists()) {
        if (info.isSymLink() || !info.isFile()) {
            error = QStringLiteral("The macro path is a symbolic link or not a regular file.");
            return {};
        }
        command_code = QStringLiteral("already_exists");
        error = QStringLiteral("A macro with that name already exists.");
        return {};
    }
    command_code.clear();
    return info.absoluteFilePath();
}

bool MacroRecorder::ReadDocument(const QJsonObject& params, EdenMacro::Document& doc, QString& file,
                                 QString& error) const {
    file = Resolve(params, error);
    if (file.isEmpty()) {
        return false;
    }
    if (!EdenMacro::Load(file, doc, error)) {
        command_code = QStringLiteral("invalid_file");
        return false;
    }
    return true;
}

QStringList MacroRecorder::LibraryFiles() const {
    QStringList files;
    const auto entries = QDir{directory}.entryInfoList(
        {QStringLiteral("*.edenmacro.json")}, QDir::Files | QDir::NoSymLinks | QDir::Readable,
        QDir::Name);
    for (const QFileInfo& entry : entries) files.append(entry.canonicalFilePath());
    for (const QString& file : linked_files) {
        if (!files.contains(file)) files.append(file);
    }
    return files;
}

bool MacroRecorder::RememberPath(const QString& file, QString& error) {
    const QFileInfo info{file};
    if ((info.canonicalPath() == QFileInfo{directory}.canonicalFilePath() &&
         file.endsWith(QStringLiteral(".edenmacro.json"))) || linked_files.contains(file)) {
        return true;
    }
    if (linked_files.size() >= 256) {
        error = QStringLiteral("The external macro library is limited to 256 files.");
        return false;
    }
    linked_files.append(file);
    WriteShortcutFile(error);
    if (!error.isEmpty()) {
        linked_files.removeLast();
        return false;
    }
    return true;
}

QJsonObject MacroRecorder::List() const {
    QJsonArray macros;
    QJsonArray errors;
    if (!directory_error.isEmpty()) {
        errors.append(directory_error);
    } else {
        for (const QString& file : LibraryFiles()) {
            const QFileInfo info{file};
            if (info.isSymLink()) {
                continue;
            }
            EdenMacro::Document doc;
            QString load_error;
            const QString canonical = info.canonicalFilePath();
            if (!EdenMacro::Load(canonical, doc, load_error)) {
                errors.append(QString{info.absoluteFilePath() + QStringLiteral(": ") + load_error});
                continue;
            }
            QJsonArray markers;
            for (const EdenMacro::Marker& marker : doc.markers) {
                markers.append(QJsonObject{{QStringLiteral("name"), marker.name},
                                           {QStringLiteral("frame"),
                                            static_cast<qint64>(marker.frame)}});
            }
            macros.append(QJsonObject{{QStringLiteral("name"), doc.name},
                                      {QStringLiteral("path"), canonical},
                                      {QStringLiteral("game_id"), doc.game_id},
                                      {QStringLiteral("player"), doc.player},
                                      {QStringLiteral("duration_frames"),
                                       static_cast<qint64>(doc.duration_frames)},
                                      {QStringLiteral("markers"), markers},
                                      {QStringLiteral("keyboard_shortcut"), doc.keyboard_shortcut},
                                      {QStringLiteral("gamepad_shortcut"), doc.gamepad_shortcut}});
        }
    }
    return QJsonObject{{QStringLiteral("directory"), directory},
                       {QStringLiteral("macros"), macros},
                       {QStringLiteral("errors"), errors}};
}

bool MacroRecorder::StartRecord(const QJsonObject& params, bool branch, QString& error) {
    command_code = QStringLiteral("invalid_params");
    if (branch) {
        if (!OnlyKeys(params,
                      {QStringLiteral("path"), QStringLiteral("name"), QStringLiteral("marker"),
                       QStringLiteral("new_name"), QStringLiteral("output_path"),
                       QStringLiteral("player"), QStringLiteral("countdown_ms")},
                      error)) {
            return false;
        }
    } else if (!OnlyKeys(params,
                         {QStringLiteral("name"), QStringLiteral("player"),
                          QStringLiteral("countdown_ms"), QStringLiteral("path")},
                         error)) {
        return false;
    }
    if (IsActive()) {
        command_code = QStringLiteral("busy");
        error = QStringLiteral("A macro is already running.");
        return false;
    }

    EdenMacro::Document next;
    QString output;
    QString marker_name;
    bool branch_take = false;
    int next_player = 0;
    int delay = 3000;
    if (branch) {
        EdenMacro::Document loaded;
        QString source_path;
        if (!ReadDocument(params, loaded, source_path, error) || source_path.isEmpty()) {
            return false;
        }
        if (!loaded.game_id.isEmpty() && loaded.game_id.compare(CurrentGameId(), Qt::CaseInsensitive) != 0) {
            error = QStringLiteral("This macro belongs to a different game.");
            return false;
        }
        if (!TakeString(params, QStringLiteral("marker"), true, marker_name, error) ||
            !CodecName(marker_name, error)) {
            command_code = QStringLiteral("invalid_params");
            return false;
        }
        QString new_name;
        if (!TakeString(params, QStringLiteral("new_name"), true, new_name, error) ||
            !FileName(new_name, error)) {
            command_code = QStringLiteral("invalid_params");
            return false;
        }
        if (!TakeInt(params, QStringLiteral("player"), 0, EdenMacro::MaxPlayer, false, loaded.player,
                     next_player, error) ||
            !TakeInt(params, QStringLiteral("countdown_ms"), 0, kCountdownMaxMs, false, 3000, delay,
                     error)) {
            command_code = QStringLiteral("invalid_params");
            return false;
        }
        if (!EdenMacro::MakePrefix(loaded, marker_name, new_name, next, error)) {
            command_code = error.contains(QStringLiteral("no marker"))
                               ? QStringLiteral("not_found")
                               : QStringLiteral("invalid_params");
            return false;
        }
        if (next.events.size() >= kRecordingEventLimit || next.duration_frames >= EdenMacro::MaxFrames) {
            error = QStringLiteral("This prefix reaches the recording memory limit; choose an earlier marker.");
            return false;
        }
        next.player = next_player;
        QString requested;
        if (!TakeString(params, QStringLiteral("output_path"), false, requested, error)) {
            command_code = QStringLiteral("invalid_params");
            return false;
        }
        output = OutputPath(new_name, requested, error);
        if (output.isEmpty()) {
            return false;
        }
        branch_take = true;
    } else {
        QString name;
        if (!TakeString(params, QStringLiteral("name"), true, name, error) || !FileName(name, error)) {
            return false;
        }
        if (!TakeInt(params, QStringLiteral("player"), 0, EdenMacro::MaxPlayer, false, 0, next_player,
                     error) ||
            !TakeInt(params, QStringLiteral("countdown_ms"), 0, kCountdownMaxMs, false, 3000, delay,
                     error)) {
            return false;
        }
        QString requested;
        if (!TakeString(params, QStringLiteral("path"), false, requested, error)) {
            return false;
        }
        output = OutputPath(name, requested, error);
        if (output.isEmpty()) {
            return false;
        }
        next.name = name;
        next.game_id = CurrentGameId();
        next.player = next_player;
    }

    if (auto* tas = input.GetTas();
        tas && std::get<0>(tas->GetStatus()) != InputCommon::TasInput::TasState::Stopped) {
        command_code = QStringLiteral("busy");
        error = QStringLiteral("TAS is running.");
        return false;
    }
    if (!GateFor(render, next_player, error)) {
        command_code = QStringLiteral("not_ready");
        return false;
    }
    if (!DevicesReady(next_player, error)) {
        command_code = QStringLiteral("not_ready");
        return false;
    }
    if (IsActive()) {
        command_code = QStringLiteral("busy");
        error = QStringLiteral("A macro is already running.");
        return false;
    }

    {
        std::scoped_lock lock{mutex};
        if (state == QStringLiteral("countdown") || state == QStringLiteral("recording") ||
            state == QStringLiteral("playing") || state == QStringLiteral("branch_playing")) {
            command_code = QStringLiteral("busy");
            error = QStringLiteral("A macro is already running.");
            return false;
        }
        source = {};
        document = std::move(next);
        path = output;
        player = next_player;
        frame = 0;
        prefix_frames = branch_take ? document.duration_frames : 0;
        branch_pending = branch_take;
        branch_marker = marker_name;
        failure.clear();
        save_pending = false;
        injected = false;
        countdown_ms = delay;
        countdown_spent = 0;
        countdown.invalidate();
        state = QStringLiteral("countdown");
        can_advance.store(false, std::memory_order_release);
        shortcuts_armed = false;
        rearm_pending = false;
    }
    if (delay == 0) {
        BeginAfterCountdown();
    }
    command_code.clear();
    return true;
}

bool MacroRecorder::Play(const QJsonObject& params, QString& error) {
    command_code = QStringLiteral("invalid_params");
    if (!OnlyKeys(params, {QStringLiteral("path"), QStringLiteral("name"), QStringLiteral("player")},
                  error)) {
        return false;
    }
    if (IsActive()) {
        command_code = QStringLiteral("busy");
        error = QStringLiteral("A macro is already running.");
        return false;
    }
    EdenMacro::Document loaded;
    QString file;
    if (!ReadDocument(params, loaded, file, error)) {
        return false;
    }
    if (!loaded.game_id.isEmpty() && loaded.game_id.compare(CurrentGameId(), Qt::CaseInsensitive) != 0) {
        error = QStringLiteral("This macro belongs to a different game.");
        return false;
    }
    int next_player = loaded.player;
    if (!TakeInt(params, QStringLiteral("player"), 0, EdenMacro::MaxPlayer, false, loaded.player,
                 next_player, error)) {
        command_code = QStringLiteral("invalid_params");
        return false;
    }
    loaded.player = next_player;
    if (auto* tas = input.GetTas();
        tas && std::get<0>(tas->GetStatus()) != InputCommon::TasInput::TasState::Stopped) {
        command_code = QStringLiteral("busy");
        error = QStringLiteral("TAS is running.");
        return false;
    }
    if (!GateFor(render, next_player, error) || !DevicesReady(next_player, error)) {
        command_code = QStringLiteral("not_ready");
        return false;
    }
    if (input.GetVirtualGamepad() == nullptr) {
        command_code = QStringLiteral("not_ready");
        error = QStringLiteral("The virtual gamepad is not available.");
        return false;
    }

    entering = true;
    emit PlaybackStarting(next_player);
    entering = false;

    {
        std::scoped_lock lock{mutex};
        if (state == QStringLiteral("countdown") || state == QStringLiteral("recording") ||
            state == QStringLiteral("playing") || state == QStringLiteral("branch_playing")) {
            command_code = QStringLiteral("busy");
            error = QStringLiteral("A macro is already running.");
            return false;
        }
        source = {};
        document = std::move(loaded);
        path = file;
        player = next_player;
        frame = 0;
        prefix_frames = 0;
        branch_pending = false;
        branch_marker.clear();
        failure.clear();
        save_pending = false;
        countdown.invalidate();
        countdown_spent = 0;
        if (auto* controller = ControllerAt(player)) {
            controller->SetPlaybackExclusive(true);
        }
        exclusive = true;
        Neutral();
        state = QStringLiteral("playing");
        shortcuts_armed = false;
        rearm_pending = false;
        QString ignored;
        can_advance.store(GateFor(render, player, ignored), std::memory_order_release);
    }
    command_code.clear();
    return true;
}

bool MacroRecorder::AddMarker(const QString& requested, QString& error) {
    std::scoped_lock lock{mutex};
    if (state != QStringLiteral("recording")) {
        error = QStringLiteral("Markers can be added while recording.");
        return false;
    }
    QString name = requested;
    if (name.isEmpty()) {
        for (int number = 1; number <= EdenMacro::MaxMarkers + 1; ++number) {
            name = QStringLiteral("Marker%1").arg(number);
            const bool used = std::any_of(document.markers.begin(), document.markers.end(),
                                           [&name](const EdenMacro::Marker& marker) {
                                               return marker.name == name;
                                           });
            if (!used) {
                break;
            }
        }
    }
    if (!CodecName(name, error)) {
        return false;
    }
    if (std::any_of(document.markers.begin(), document.markers.end(),
                    [&name](const EdenMacro::Marker& marker) { return marker.name == name; })) {
        error = QStringLiteral("That marker name already exists.");
        return false;
    }
    if (document.markers.size() >= static_cast<std::size_t>(EdenMacro::MaxMarkers)) {
        error = QStringLiteral("The macro already has 1024 markers.");
        return false;
    }
    document.markers.push_back(EdenMacro::Marker{name, frame});
    return true;
}

bool MacroRecorder::Bind(const QJsonObject& params, QString& error) {
    command_code = QStringLiteral("invalid_params");
    if (!OnlyKeys(params,
                  {QStringLiteral("path"), QStringLiteral("name"), QStringLiteral("keyboard"),
                   QStringLiteral("gamepad")},
                  error)) {
        return false;
    }
    if (IsActive()) {
        command_code = QStringLiteral("busy");
        error = QStringLiteral("A macro is already running.");
        return false;
    }
    EdenMacro::Document doc;
    QString file;
    if (!ReadDocument(params, doc, file, error)) {
        return false;
    }
    QString keyboard;
    QString gamepad;
    if (!TakeString(params, QStringLiteral("keyboard"), true, keyboard, error) ||
        !TakeString(params, QStringLiteral("gamepad"), true, gamepad, error)) {
        command_code = QStringLiteral("invalid_params");
        return false;
    }
    QKeySequence sequence;
    if (!keyboard.isEmpty()) {
        if (!CodecText(keyboard, EdenMacro::MaxShortcutChars, error)) {
            return false;
        }
        sequence = QKeySequence::fromString(keyboard, QKeySequence::PortableText);
        if (sequence.isEmpty()) {
            error = QStringLiteral("The keyboard shortcut is not a valid key sequence.");
            return false;
        }
        keyboard = sequence.toString(QKeySequence::PortableText);
    }
    quint32 buttons = 0;
    if (!ParseGamepad(gamepad, buttons, error)) {
        return false;
    }
    const QShortcut* previous = nullptr;
    for (const Binding& binding : bindings) {
        if (binding.path == file) previous = binding.keyboard.data();
    }
    if (!KeyboardAvailable(sequence, owner, previous, error)) return false;
    for (const QString& other_path : LibraryFiles()) {
        if (other_path == file) continue;
        EdenMacro::Document other;
        QString ignored;
        quint32 other_buttons = 0;
        if (!EdenMacro::Load(other_path, other, ignored) || other.player != doc.player ||
            !ParseGamepad(other.gamepad_shortcut, other_buttons, ignored)) continue;
        if (buttons && other_buttons &&
            ((buttons & other_buttons) == buttons || (buttons & other_buttons) == other_buttons)) {
            error = QStringLiteral("That gamepad combination overlaps the playback shortcut for %1.").arg(other.name);
            return false;
        }
    }
    (void)buttons;
    doc.keyboard_shortcut = keyboard;
    doc.gamepad_shortcut = gamepad.trimmed();
    if (!RememberPath(file, error)) {
        command_code = QStringLiteral("io_error");
        return false;
    }
    if (!EdenMacro::Save(file, doc, error, true)) {
        command_code = QStringLiteral("io_error");
        return false;
    }
    ReloadBindings();
    command_code.clear();
    return true;
}

bool MacroRecorder::SetShortcuts(const QJsonObject& params, QString& error) {
    command_code = QStringLiteral("invalid_params");
    if (!OnlyKeys(params, {QStringLiteral("marker_keyboard"), QStringLiteral("marker_gamepad")},
                  error)) {
        return false;
    }
    const bool set_keyboard = params.contains(QStringLiteral("marker_keyboard"));
    const bool set_gamepad = params.contains(QStringLiteral("marker_gamepad"));
    if (!set_keyboard && !set_gamepad) {
        command_code.clear();
        return true;
    }
    QString keyboard = marker_keyboard;
    QString gamepad = marker_gamepad;
    quint32 buttons = marker_buttons;
    if (set_keyboard) {
        if (!TakeString(params, QStringLiteral("marker_keyboard"), true, keyboard, error)) {
            return false;
        }
        if (keyboard.isEmpty()) {
            keyboard.clear();
        } else {
            if (!CodecText(keyboard, EdenMacro::MaxShortcutChars, error)) {
                return false;
            }
            const QKeySequence sequence =
                QKeySequence::fromString(keyboard, QKeySequence::PortableText);
            if (sequence.isEmpty()) {
                error = QStringLiteral("The keyboard shortcut is not a valid key sequence.");
                return false;
            }
            keyboard = sequence.toString(QKeySequence::PortableText);
        }
    }
    if (set_gamepad) {
        if (!TakeString(params, QStringLiteral("marker_gamepad"), true, gamepad, error) ||
            !ParseGamepad(gamepad, buttons, error)) {
            return false;
        }
        gamepad = gamepad.trimmed();
    }
    const QString old_keyboard = marker_keyboard;
    if (!KeyboardAvailable(QKeySequence::fromString(keyboard, QKeySequence::PortableText),
                           owner, marker_shortcut.data(), error)) return false;
    const QString old_gamepad = marker_gamepad;
    const quint32 old_buttons = marker_buttons;
    marker_keyboard = keyboard;
    marker_gamepad = gamepad;
    marker_buttons = buttons;
    WriteShortcutFile(error);
    if (!error.isEmpty()) {
        marker_keyboard = old_keyboard;
        marker_gamepad = old_gamepad;
        marker_buttons = old_buttons;
        command_code = QStringLiteral("io_error");
        return false;
    }
    if (marker_shortcut) {
        delete marker_shortcut.data();
    }
    const QKeySequence sequence =
        QKeySequence::fromString(marker_keyboard, QKeySequence::PortableText);
    if (!sequence.isEmpty()) {
        marker_shortcut = MakeShortcut(sequence, owner);
        QObject::connect(marker_shortcut, &QShortcut::activated, this, [this] {
            QString marker_error;
            if (AddMarker({}, marker_error)) {
                NoteUi();
            }
        });
    }
    command_code.clear();
    return true;
}

void MacroRecorder::ReloadBindings() {
    for (Binding& binding : bindings) {
        if (binding.keyboard) {
            delete binding.keyboard.data();
        }
    }
    bindings.clear();
    if (directory_error.isEmpty()) {
        for (const QString& file_path : LibraryFiles()) {
            const QFileInfo info{file_path};
            EdenMacro::Document doc;
            QString load_error;
            const QString canonical = info.canonicalFilePath();
            if (canonical.isEmpty() || !EdenMacro::Load(canonical, doc, load_error)) {
                continue;
            }
            Binding binding;
            binding.path = canonical;
            binding.player = doc.player;
            QString parse_error;
            if (!ParseGamepad(doc.gamepad_shortcut, binding.buttons, parse_error)) {
                binding.buttons = 0;
            }
            if (!doc.keyboard_shortcut.isEmpty()) {
                const QKeySequence sequence = QKeySequence::fromString(
                    doc.keyboard_shortcut, QKeySequence::PortableText);
                if (!sequence.isEmpty()) {
                    binding.keyboard = MakeShortcut(sequence, owner);
                    const QString file = binding.path;
                    QObject::connect(binding.keyboard, &QShortcut::activated, this, [this, file] {
                        Handle(QStringLiteral("macro.play"),
                               QJsonObject{{QStringLiteral("path"), file}});
                        NoteUi();
                    });
                }
            }
            bindings.push_back(std::move(binding));
        }
    }
    if (marker_shortcut) {
        delete marker_shortcut.data();
    }
    const QKeySequence sequence =
        QKeySequence::fromString(marker_keyboard, QKeySequence::PortableText);
    if (!sequence.isEmpty()) {
        marker_shortcut = MakeShortcut(sequence, owner);
        QObject::connect(marker_shortcut, &QShortcut::activated, this, [this] {
            QString marker_error;
            if (AddMarker({}, marker_error)) {
                NoteUi();
            }
        });
    }
    ApplyShortcutGates();
}

void MacroRecorder::Tick() {
    bool begin = false;
    bool flush = false;
    int refresh_player = -1;
    {
        std::scoped_lock lock{mutex};
        if (state == QStringLiteral("countdown")) {
            QString ignored;
            if (!Ready(ignored)) {
                if (countdown.isValid()) {
                    countdown_spent += countdown.elapsed();
                    countdown.invalidate();
                }
            } else {
                if (!countdown.isValid()) {
                    countdown.start();
                }
                if (countdown_spent + countdown.elapsed() >= countdown_ms) {
                    begin = true;
                }
            }
        }
        QString ignored;
        const bool gate = Ready(ignored);
        const bool advancing = state == QStringLiteral("recording") ||
                               state == QStringLiteral("playing") ||
                               state == QStringLiteral("branch_playing");
        if (!save_pending) {
            can_advance.store(advancing && gate, std::memory_order_release);
        }
        flush = save_pending;
        if (refresh_user_pending) {
            refresh_user_pending = false;
            refresh_player = player;
        }
    }
    if (refresh_player >= 0) {
        if (auto* controller = ControllerAt(refresh_player)) {
            controller->RefreshUserInput();
        }
    }
    if (begin) {
        BeginAfterCountdown();
    }
    if (flush) {
        Finish();
    }
    PollChords();
    NoteUi();
}

void MacroRecorder::Finish() {
    EdenMacro::Document copy;
    QString file;
    QString prior_failure;
    bool write = false;
    {
        std::scoped_lock lock{mutex};
        if (!save_pending && state != QStringLiteral("recording")) {
            return;
        }
        can_advance.store(false, std::memory_order_release);
        if (injected) {
            Neutral();
        }
        ReleaseExclusive();
        countdown.invalidate();
        countdown_spent = 0;
        save_pending = false;
        prior_failure = failure;
        rearm_pending = true;
        shortcuts_armed = false;
        if (document.events.empty() || document.duration_frames < 1) {
            state = QStringLiteral("error");
            failure = QStringLiteral("No frames were captured.");
            branch_pending = false;
        } else {
            copy = document;
            file = path;
            write = true;
            state = QStringLiteral("finished");
            failure.clear();
            branch_pending = false;
        }
    }
    if (write) {
        QString save_error;
        if (!EdenMacro::Save(file, copy, save_error, false)) {
            std::scoped_lock lock{mutex};
            state = QStringLiteral("error");
            failure = save_error;
            LOG_ERROR(Frontend, "Macro save failed: {}", save_error.toStdString());
        } else if (!RememberPath(QFileInfo{file}.canonicalFilePath(), save_error)) {
            std::scoped_lock lock{mutex};
            state = QStringLiteral("error");
            failure = QStringLiteral("Macro saved, but library registration failed: %1").arg(save_error);
        } else if (!prior_failure.isEmpty()) {
            std::scoped_lock lock{mutex};
            state = QStringLiteral("error");
            failure = prior_failure;
        }
    }
    if (!shutting_down) {
        NoteUi();
    }
}

void MacroRecorder::Cancel() {
    {
        std::scoped_lock lock{mutex};
        const bool live = state == QStringLiteral("countdown") || state == QStringLiteral("playing") ||
                          state == QStringLiteral("branch_playing");
        if (!live) {
            return;
        }
        const bool was_playback =
            state == QStringLiteral("playing") || state == QStringLiteral("branch_playing");
        can_advance.store(false, std::memory_order_release);
        if (injected) {
            Neutral();
        }
        ReleaseExclusive();
        state = QStringLiteral("idle");
        failure.clear();
        branch_pending = false;
        save_pending = false;
        countdown.invalidate();
        countdown_spent = 0;
        rearm_pending = was_playback;
        shortcuts_armed = !was_playback;
    }
    if (!shutting_down) {
        NoteUi();
    }
}

void MacroRecorder::BeginAfterCountdown() {
    bool branch = false;
    {
        std::scoped_lock lock{mutex};
        if (state != QStringLiteral("countdown")) {
            return;
        }
        branch = branch_pending;
    }
    if (branch) {
        entering = true;
        emit PlaybackStarting(player);
        entering = false;
    }
    {
        std::scoped_lock lock{mutex};
        if (state != QStringLiteral("countdown")) {
            return;
        }
        countdown.invalidate();
        countdown_spent = 0;
        frame = 0;
        failure.clear();
        if (branch_pending) {
            prefix_frames = document.duration_frames;
            if (auto* controller = ControllerAt(player)) {
                controller->SetPlaybackExclusive(true);
            }
            exclusive = true;
            Neutral();
            state = QStringLiteral("branch_playing");
            shortcuts_armed = false;
            rearm_pending = false;
        } else {
            prefix_frames = 0;
            state = QStringLiteral("recording");
            shortcuts_armed = true;
            rearm_pending = false;
        }
        QString ignored;
        can_advance.store(GateFor(render, player, ignored), std::memory_order_release);
    }
    if (!shutting_down) {
        NoteUi();
    }
}

void MacroRecorder::Neutral() {
    auto* pad = input.GetVirtualGamepad();
    injected = false;
    if (pad == nullptr || player < 0 || player > EdenMacro::MaxPlayer) {
        return;
    }
    const auto port = static_cast<std::size_t>(player);
    for (int button = 0; button < EdenMacro::ButtonCount; ++button) {
        pad->SetButtonState(port, button, false);
    }
    pad->SetStickPosition(port, 0, 0.F, 0.F);
    pad->SetStickPosition(port, 1, 0.F, 0.F);
    pad->SetTriggerAxis(port, 0, 0.F);
    pad->SetTriggerAxis(port, 1, 0.F);
    for (int sensor = 0; sensor < 2; ++sensor) {
        pad->SetMotionSensor(port, sensor, 10000, 0.F, 0.F, 0.F, 0.F, 0.F, -1.F);
    }
}

void MacroRecorder::ReleaseExclusive() {
    if (!exclusive) {
        return;
    }
    if (auto* controller = ControllerAt(player)) {
        controller->SetPlaybackExclusive(false);
    }
    exclusive = false;
}

void MacroRecorder::RecordOne() {
    auto* controller = ControllerAt(player);
    if (!controller || !controller->InputDevicesLoaded()) {
        can_advance.store(false, std::memory_order_release);
        if (!document.events.empty() && document.duration_frames >= 1) {
            save_pending = true;
            failure = QStringLiteral("Controller input is not loaded.");
        } else {
            state = QStringLiteral("error");
            failure = QStringLiteral("Controller input is not loaded.");
            if (injected) {
                Neutral();
            }
            ReleaseExclusive();
            rearm_pending = true;
            shortcuts_armed = false;
        }
        return;
    }
    if (save_pending || frame >= EdenMacro::MaxFrames ||
        document.events.size() >= kRecordingEventLimit) {
        save_pending = true;
        can_advance.store(false, std::memory_order_release);
        return;
    }
    const EdenMacro::Sample sample = Capture(player);
    if (document.events.empty() || sample != document.events.back().sample) {
        document.events.push_back(EdenMacro::Event{frame, sample});
    }
    ++frame;
    document.duration_frames = frame;
    if (frame >= EdenMacro::MaxFrames ||
        document.events.size() >= kRecordingEventLimit) {
        save_pending = true;
        can_advance.store(false, std::memory_order_release);
    }
}

void MacroRecorder::PlayOne() {
    auto* controller = ControllerAt(player);
    if (input.GetVirtualGamepad() == nullptr || controller == nullptr ||
        !controller->InputDevicesLoaded()) {
        if (injected) {
            Neutral();
        }
        ReleaseExclusive();
        can_advance.store(false, std::memory_order_release);
        state = QStringLiteral("error");
        failure = input.GetVirtualGamepad() == nullptr
                      ? QStringLiteral("The virtual gamepad is not available.")
                      : QStringLiteral("Controller input is not loaded.");
        rearm_pending = true;
        shortcuts_armed = false;
        return;
    }
    if (state == QStringLiteral("branch_playing") && frame >= prefix_frames) {
        Neutral();
        ReleaseExclusive();
        state = QStringLiteral("recording");
        shortcuts_armed = true;
        rearm_pending = false;
        refresh_user_pending = true;
        RecordOne();
        return;
    }
    if (frame >= document.duration_frames) {
        if (injected) {
            Neutral();
        }
        ReleaseExclusive();
        can_advance.store(false, std::memory_order_release);
        state = QStringLiteral("finished");
        failure.clear();
        rearm_pending = true;
        shortcuts_armed = false;
        return;
    }
    if (!Apply(EdenMacro::SampleAt(document, frame))) {
        ReleaseExclusive();
        can_advance.store(false, std::memory_order_release);
        state = QStringLiteral("error");
        failure = QStringLiteral("The virtual gamepad is not available.");
        rearm_pending = true;
        shortcuts_armed = false;
        return;
    }
    ++frame;
}

EdenMacro::Sample MacroRecorder::Capture(int index) const {
    EdenMacro::Sample sample;
    auto* controller = ControllerAt(index);
    if (!controller || !controller->InputDevicesLoaded()) {
        return sample;
    }
    const Core::HID::MappedInputSnapshot snapshot = controller->MappedSnapshot(true);
    const int buttons = std::min(EdenMacro::ButtonCount, static_cast<int>(snapshot.buttons.size()));
    for (int button = 0; button < buttons; ++button) {
        if (snapshot.buttons[static_cast<std::size_t>(button)].value) {
            sample.buttons |= quint32{1} << button;
        }
    }
    if (snapshot.sticks.size() >= 2) {
        sample.sticks = {Clamp(snapshot.sticks[0].x.value, -1.F, 1.F),
                         Clamp(snapshot.sticks[0].y.value, -1.F, 1.F),
                         Clamp(snapshot.sticks[1].x.value, -1.F, 1.F),
                         Clamp(snapshot.sticks[1].y.value, -1.F, 1.F)};
    }
    if (snapshot.triggers.size() >= 2) {
        sample.triggers = {Clamp(snapshot.triggers[0].analog.value, 0.F, 1.F),
                           Clamp(snapshot.triggers[1].analog.value, 0.F, 1.F)};
    }
    for (std::size_t sensor = 0; sensor < sample.motion.size() && sensor < snapshot.motion.size();
         ++sensor) {
        const Common::Input::MotionStatus& motion = snapshot.motion[sensor];
        const u64 delta = motion.delta_timestamp;
        sample.motion[sensor].delta_us =
            delta >= static_cast<u64>(EdenMacro::MinMotionDeltaUs) &&
                    delta <= static_cast<u64>(EdenMacro::MaxMotionDeltaUs)
                ? static_cast<qint64>(delta)
                : sample.motion[sensor].delta_us;
        sample.motion[sensor].gyro = {Clamp(motion.gyro.x.value, -100.F, 100.F),
                                      Clamp(motion.gyro.y.value, -100.F, 100.F),
                                      Clamp(motion.gyro.z.value, -100.F, 100.F)};
        sample.motion[sensor].accel = {Clamp(motion.accel.x.value, -100.F, 100.F),
                                       Clamp(motion.accel.y.value, -100.F, 100.F),
                                       Clamp(motion.accel.z.value, -100.F, 100.F)};
    }
    return sample;
}

bool MacroRecorder::Apply(const EdenMacro::Sample& sample) {
    auto* pad = input.GetVirtualGamepad();
    if (pad == nullptr || player < 0 || player > EdenMacro::MaxPlayer) {
        return false;
    }
    const auto port = static_cast<std::size_t>(player);
    for (int button = 0; button < EdenMacro::ButtonCount; ++button) {
        const bool pressed = (sample.buttons & (quint32{1} << button)) != 0;
        pad->SetButtonState(port, button, pressed);
    }
    pad->SetStickPosition(port, 0, sample.sticks[0], sample.sticks[1]);
    pad->SetStickPosition(port, 1, sample.sticks[2], sample.sticks[3]);
    pad->SetTriggerAxis(port, 0, sample.triggers[0]);
    pad->SetTriggerAxis(port, 1, sample.triggers[1]);
    for (int sensor = 0; sensor < 2; ++sensor) {
        const EdenMacro::Motion& motion = sample.motion[static_cast<std::size_t>(sensor)];
        pad->SetMotionSensor(port, sensor, static_cast<u64>(motion.delta_us), motion.gyro[0],
                             motion.gyro[1], motion.gyro[2], motion.accel[0], motion.accel[1],
                             motion.accel[2]);
    }
    injected = true;
    return true;
}

bool MacroRecorder::ParseGamepad(const QString& text, quint32& buttons, QString& error) const {
    buttons = 0;
    if (!CodecText(text, EdenMacro::MaxShortcutChars, error)) {
        return false;
    }
    if (text.trimmed().isEmpty()) {
        return true;
    }
    bool any = false;
    const QStringList parts = text.split(QLatin1Char('+'));
    for (const QString& part : parts) {
        if (part.trimmed().isEmpty()) {
            continue;
        }
        const int bit = ButtonBit(part);
        if (bit < 0 || bit >= EdenMacro::ButtonCount) {
            error = QStringLiteral("Unknown gamepad button \"%1\".").arg(part.trimmed());
            return false;
        }
        buttons |= quint32{1} << bit;
        any = true;
    }
    if (!any) {
        buttons = 0;
    }
    return true;
}

void MacroRecorder::ShowIndicator(const QJsonObject& status) {
    if (!indicator) {
        return;
    }
    const QString cue = status.value(QStringLiteral("cue")).toString();
    if (cue.isEmpty() || cue == QStringLiteral("none")) {
        indicator->hide();
        return;
    }
    QString text;
    QString color = QStringLiteral("#f7f7f7");
    if (cue == QStringLiteral("countdown")) {
        const qint64 remaining =
            static_cast<qint64>(status.value(QStringLiteral("countdown_remaining")).toDouble());
        const int seconds = std::max(1, static_cast<int>((remaining + 999) / 1000));
        text = QString::number(seconds);
    } else if (cue == QStringLiteral("red")) {
        text = QStringLiteral("REC");
        color = QStringLiteral("#ff3b30");
    } else {
        text = QStringLiteral("PLAY");
        color = QStringLiteral("#4da3ff");
    }
    indicator->setText(text);
    indicator->setStyleSheet(
        QStringLiteral("QLabel { color: %1; background-color: rgba(0, 0, 0, 170); "
                       "font-size: 28px; font-weight: 700; padding: 6px 14px; }")
            .arg(color));
    indicator->adjustSize();
    const int x = std::max(0, (render.width() - indicator->width()) / 2);
    indicator->move(render.mapToGlobal(QPoint(x, 12)));
    indicator->show();
}

void MacroRecorder::PollChords() {
    bool refresh = false;
    std::vector<int> ports;
    {
        std::scoped_lock lock{mutex};
        refresh = rearm_pending;
        if (refresh) {
            ports.push_back(player);
            ports.push_back(0);
            ports.push_back(8);
            for (const Binding& binding : bindings) {
                ports.push_back(binding.player);
            }
        }
    }
    if (refresh) {
        std::sort(ports.begin(), ports.end());
        ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
        for (const int index : ports) {
            if (auto* controller = ControllerAt(index); controller && controller->InputDevicesLoaded()) {
                controller->RefreshUserInput();
            }
        }
    }

    QString play_path;
    bool mark = false;
    {
        std::scoped_lock lock{mutex};
        const auto chord = [this](quint32 buttons, int index) {
            return buttons != 0 && (ButtonMask(index) & buttons) == buttons;
        };
        if (rearm_pending) {
            bool any = chord(marker_buttons, player) || chord(marker_buttons, 0) ||
                       chord(marker_buttons, 8);
            marker_held = any;
            for (Binding& binding : bindings) {
                const bool held = chord(binding.buttons, binding.player);
                binding.held = held;
                any = any || held;
            }
            if (!any) {
                rearm_pending = false;
                shortcuts_armed = true;
                marker_held = false;
                for (Binding& binding : bindings) {
                    binding.held = false;
                }
            }
            return;
        }
        QString ignored;
        const bool gate = Ready(ignored);
        if (state == QStringLiteral("countdown")) {
            marker_held = chord(marker_buttons, player) || chord(marker_buttons, 0) ||
                          chord(marker_buttons, 8);
            for (Binding& binding : bindings) {
                binding.held = chord(binding.buttons, binding.player);
            }
            return;
        }
        if (!shortcuts_armed || QApplication::applicationState() != Qt::ApplicationActive) {
            return;
        }
        if (!gate) {
            if (state == QStringLiteral("recording")) {
                marker_held = chord(marker_buttons, player);
            } else if (state == QStringLiteral("idle") || state == QStringLiteral("finished") ||
                       state == QStringLiteral("error")) {
                marker_held = chord(marker_buttons, 0) || chord(marker_buttons, 8);
                for (Binding& binding : bindings) {
                    binding.held = chord(binding.buttons, binding.player);
                }
            }
            return;
        }
        if (state == QStringLiteral("recording")) {
            const bool held = chord(marker_buttons, player);
            if (held && !marker_held) {
                marker_held = true;
                mark = true;
            } else if (!held) {
                marker_held = false;
            }
        } else if (state == QStringLiteral("idle") || state == QStringLiteral("finished") ||
                   state == QStringLiteral("error")) {
            marker_held = chord(marker_buttons, 0) || chord(marker_buttons, 8);
            for (Binding& binding : bindings) {
                if (binding.buttons == 0) {
                    binding.held = false;
                    continue;
                }
                const bool held = chord(binding.buttons, binding.player);
                if (held && !binding.held) {
                    binding.held = true;
                    if (play_path.isEmpty()) {
                        play_path = binding.path;
                    }
                } else if (!held) {
                    binding.held = false;
                }
            }
        }
    }
    if (mark) {
        QString marker_error;
        AddMarker({}, marker_error);
    }
    if (!play_path.isEmpty()) {
        Handle(QStringLiteral("macro.play"), QJsonObject{{QStringLiteral("path"), play_path}});
    }
}

void MacroRecorder::ApplyShortcutGates() {
    QString current;
    bool rearm = false;
    bool keyboard_ready = false;
    {
        std::scoped_lock lock{mutex};
        current = state;
        rearm = rearm_pending;
        keyboard_ready = !marker_keyboard.isEmpty();
    }
    QString ignored;
    const bool gate = GateFor(render, player, ignored);
    const bool idle = current == QStringLiteral("idle") || current == QStringLiteral("finished") ||
                      current == QStringLiteral("error");
    const bool recording = current == QStringLiteral("recording");
    const bool block = rearm || !gate || current == QStringLiteral("playing") ||
                       current == QStringLiteral("branch_playing") ||
                       current == QStringLiteral("countdown");
    if (marker_shortcut) {
        marker_shortcut->setEnabled(!block && recording && keyboard_ready);
    }
    for (Binding& binding : bindings) {
        if (binding.keyboard) {
            binding.keyboard->setEnabled(!block && idle);
        }
    }
    if (stop_shortcut) {
        const bool live = current == QStringLiteral("countdown") || recording ||
                          current == QStringLiteral("playing") ||
                          current == QStringLiteral("branch_playing");
        stop_shortcut->setEnabled(live);
    }
}

void MacroRecorder::PublishChange() {
    QString current;
    QString cue;
    quint64 current_frame = 0;
    qint64 remaining = 0;
    {
        std::scoped_lock lock{mutex};
        current = state;
        cue = Cue();
        current_frame = frame;
        remaining = CountdownRemaining();
    }
    const int countdown_key = static_cast<int>(std::min<qint64>(remaining, kCountdownMaxMs));
    if (current == ui_state && cue == ui_cue && current_frame == ui_frame &&
        countdown_key == ui_countdown) {
        return;
    }
    ui_state = current;
    ui_cue = cue;
    ui_frame = current_frame;
    ui_countdown = countdown_key;
    if (!shutting_down) {
        emit Changed();
    }
}

void MacroRecorder::NoteUi() {
    if (shutting_down) {
        return;
    }
    ApplyShortcutGates();
    ShowIndicator(Status());
    PublishChange();
}

void MacroRecorder::LoadShortcutFile() {
    marker_keyboard = QStringLiteral("Ctrl+Shift+M");
    marker_gamepad.clear();
    marker_buttons = 0;
    if (directory_error.isEmpty()) {
        const QString file = directory + QStringLiteral("/marker_shortcuts.json");
        QFile input_file{file};
        if (input_file.exists()) {
            const QFileInfo info{file};
            if (info.isSymLink() || !info.isFile() || info.size() > 65536 ||
                !input_file.open(QIODevice::ReadOnly)) {
                LOG_ERROR(Frontend, "Ignoring unreadable marker shortcut file {}", file.toStdString());
            } else {
                QJsonParseError parse_error;
                const QJsonDocument json = QJsonDocument::fromJson(input_file.read(65537), &parse_error);
                const QJsonObject object = json.object();
                if (parse_error.error != QJsonParseError::NoError || !json.isObject() ||
                    !object.value(QStringLiteral("marker_keyboard")).isString() ||
                    !object.value(QStringLiteral("marker_gamepad")).isString()) {
                    LOG_ERROR(Frontend, "Ignoring invalid marker shortcut file {}", file.toStdString());
                } else {
                    const QJsonArray files = object.value(QStringLiteral("files")).toArray();
                    for (const QJsonValue& value : files) {
                        if (linked_files.size() >= 256) break;
                        const QString linked_path = value.toString();
                        if (linked_path.size() <= 4096 && QDir::isAbsolutePath(linked_path) && !linked_files.contains(linked_path)) {
                            linked_files.append(linked_path);
                        }
                    }
                    const QString keyboard = object.value(QStringLiteral("marker_keyboard")).toString();
                    const QString gamepad = object.value(QStringLiteral("marker_gamepad")).toString();
                    QString text_error;
                    quint32 buttons = 0;
                    const QKeySequence sequence =
                        QKeySequence::fromString(keyboard, QKeySequence::PortableText);
                    if (CodecText(keyboard, EdenMacro::MaxShortcutChars, text_error) &&
                        (keyboard.isEmpty() || !sequence.isEmpty()) &&
                        ParseGamepad(gamepad, buttons, text_error)) {
                        marker_keyboard =
                            keyboard.isEmpty() ? QString{} : sequence.toString(QKeySequence::PortableText);
                        marker_gamepad = gamepad.trimmed();
                        marker_buttons = buttons;
                    } else {
                        LOG_ERROR(Frontend, "Ignoring invalid marker shortcut values in {}",
                                  file.toStdString());
                    }
                }
            }
        }
    }
    if (marker_shortcut) {
        delete marker_shortcut.data();
    }
    const QKeySequence sequence =
        QKeySequence::fromString(marker_keyboard, QKeySequence::PortableText);
    if (!sequence.isEmpty()) {
        marker_shortcut = MakeShortcut(sequence, owner);
        QObject::connect(marker_shortcut, &QShortcut::activated, this, [this] {
            QString marker_error;
            if (AddMarker({}, marker_error)) {
                NoteUi();
            }
        });
    }
}

void MacroRecorder::WriteShortcutFile(QString& error) const {
    if (!directory_error.isEmpty()) {
        error = directory_error;
        return;
    }
    const QString file_path = directory + QStringLiteral("/marker_shortcuts.json");
    const QFileInfo info{file_path};
    if (info.exists() && (info.isSymLink() || !info.isFile())) {
        error = QStringLiteral("The marker shortcut file is a symbolic link or not a regular file.");
        return;
    }
    QSaveFile file{file_path};
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return;
    }
    const QJsonObject object{{QStringLiteral("files"), QJsonArray::fromStringList(linked_files)},
                             {QStringLiteral("marker_keyboard"), marker_keyboard},
                             {QStringLiteral("marker_gamepad"), marker_gamepad}};
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (bytes.size() > 65536) {
        error = QStringLiteral("Macro shortcut settings exceed the 64 KiB limit.");
        file.cancelWriting();
        return;
    }
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        error = file.errorString();
        return;
    }
    if (!file.commit()) {
        error = file.errorString();
        return;
    }
    error.clear();
}

quint32 MacroRecorder::ButtonMask(int index) const {
    auto* controller = ControllerAt(index);
    if (!controller || !controller->InputDevicesLoaded()) {
        return 0;
    }
    const Core::HID::MappedInputSnapshot snapshot = controller->MappedSnapshot();
    quint32 mask = 0;
    const int count = std::min(EdenMacro::ButtonCount, static_cast<int>(snapshot.buttons.size()));
    for (int button = 0; button < count; ++button) {
        if (snapshot.buttons[static_cast<std::size_t>(button)].value) {
            mask |= quint32{1} << button;
        }
    }
    return mask;
}

qint64 MacroRecorder::CountdownRemaining() const {
    if (state != QStringLiteral("countdown")) {
        return 0;
    }
    const qint64 running = countdown.isValid() ? countdown.elapsed() : 0;
    const qint64 used = countdown_spent + running;
    if (used >= countdown_ms) {
        return 0;
    }
    return static_cast<qint64>(countdown_ms) - used;
}

QString MacroRecorder::Cue() const {
    if (state == QStringLiteral("countdown")) {
        return QStringLiteral("countdown");
    }
    if (state == QStringLiteral("recording")) {
        return QStringLiteral("red");
    }
    if (state == QStringLiteral("playing") || state == QStringLiteral("branch_playing")) {
        return QStringLiteral("blue");
    }
    return QStringLiteral("none");
}
