// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <QJsonArray>
#include <QStringList>

#include "common/settings.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_util.h"
#include "input_common/drivers/touch_screen.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "qt_common/qt_common.h"
#include "yuzu/control_input.h"
#include "yuzu/control_server.h"

namespace {
using CS = ControlServer;
const QStringList ButtonNames{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("x"), QStringLiteral("y"),
    QStringLiteral("lstick"), QStringLiteral("rstick"), QStringLiteral("l"), QStringLiteral("r"),
    QStringLiteral("zl"), QStringLiteral("zr"), QStringLiteral("plus"), QStringLiteral("minus"),
    QStringLiteral("left"), QStringLiteral("up"), QStringLiteral("right"), QStringLiteral("down"),
    QStringLiteral("sl_left"), QStringLiteral("sr_left"), QStringLiteral("home"), QStringLiteral("capture"),
    QStringLiteral("sl_right"), QStringLiteral("sr_right")};
constexpr std::size_t TouchBase = 0x10000;
constexpr s32 HidMax = 0x7fff;
constexpr int TriggerButtonBase = 32;
constexpr int TriggerAxisBase = 6;
constexpr int RingAxisX = 4;
constexpr float TriggerPress = 0.5f;

bool Integer(const QJsonValue& value, int low, int high) {
    return value.isDouble() && std::isfinite(value.toDouble()) &&
           std::trunc(value.toDouble()) == value.toDouble() && value.toDouble() >= low && value.toDouble() <= high;
}
bool Number(const QJsonValue& value, double low, double high) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= low && value.toDouble() <= high;
}
QJsonValue Default(const QJsonObject& object, const QString& key, int value) {
    return object.contains(key) ? object.value(key) : QJsonValue{value};
}
PadIdentifier Identifier(std::size_t player) {
    return {.guid = Common::UUID{}, .port = player, .pad = 0};
}
bool Same(float left, float right) {
    return left == right;
}
bool NearStick(s32 got, float requested) {
    return std::abs(got - static_cast<s32>(requested * static_cast<float>(HidMax))) <= 2;
}
bool Near(float got, float requested) {
    return std::abs(got - requested) <= 0.02f;
}

const Core::HID::EmulatedController* ControllerFor(std::size_t player) {
    if (!QtCommon::system) {
        return nullptr;
    }
    return QtCommon::system->HIDCore().GetEmulatedController(Service::HID::IndexToNpadIdType(player));
}

QString ButtonStyleGate(Core::HID::NpadStyleIndex style, int button) {
    using S = Core::HID::NpadStyleIndex;
    namespace B = Settings::NativeButton;
    if (button == B::Home || button == B::Screenshot) {
        return {};
    }
    if (style == S::GameCube && (button == B::ZL || button == B::ZR)) {
        return QStringLiteral("gamecube_digital_trigger");
    }
    if (style == S::GameCube && button == B::L) {
        return QStringLiteral("gamecube_l_overwritten");
    }
    const bool left_shoulder = button == B::SLLeft || button == B::SRLeft;
    const bool right_shoulder = button == B::SLRight || button == B::SRRight;
    if (left_shoulder && style != S::JoyconLeft && style != S::JoyconDual) {
        return QStringLiteral("style_masked");
    }
    if (right_shoulder && style != S::JoyconRight && style != S::JoyconDual) {
        return QStringLiteral("style_masked");
    }
    const bool right = button == B::A || button == B::B || button == B::X || button == B::Y ||
                       button == B::RStick || button == B::R || button == B::ZR || button == B::Plus;
    const bool left = button == B::DLeft || button == B::DUp || button == B::DRight || button == B::DDown ||
                      button == B::LStick || button == B::L || button == B::ZL || button == B::Minus;
    if (style == S::JoyconLeft && right) {
        return QStringLiteral("style_masked");
    }
    if (style == S::JoyconRight && left) {
        return QStringLiteral("style_masked");
    }
    return {};
}

bool MotionPublished(Core::HID::NpadStyleIndex style, int sensor) {
    using S = Core::HID::NpadStyleIndex;
    switch (style) {
    case S::Fullkey:
    case S::Handheld:
    case S::JoyconLeft:
    case S::Pokeball:
        return sensor == 0;
    case S::JoyconRight:
        return sensor == 1;
    case S::JoyconDual:
        return sensor == 0 || sensor == 1;
    default:
        return false;
    }
}

bool ButtonFrontend(const Core::HID::EmulatedController* controller, int button, bool pressed) {
    namespace B = Settings::NativeButton;
    if (button == B::Home) {
        return static_cast<bool>(controller->GetHomeButtons().home) == pressed;
    }
    if (button == B::Screenshot) {
        return static_cast<bool>(controller->GetCaptureButtons().capture) == pressed;
    }
    const auto state = controller->GetNpadButtons();
    bool got = false;
    switch (button) {
    case B::A: got = static_cast<bool>(state.a); break;
    case B::B: got = static_cast<bool>(state.b); break;
    case B::X: got = static_cast<bool>(state.x); break;
    case B::Y: got = static_cast<bool>(state.y); break;
    case B::LStick: got = static_cast<bool>(state.stick_l); break;
    case B::RStick: got = static_cast<bool>(state.stick_r); break;
    case B::L: got = static_cast<bool>(state.l); break;
    case B::R: got = static_cast<bool>(state.r); break;
    case B::ZL: got = static_cast<bool>(state.zl); break;
    case B::ZR: got = static_cast<bool>(state.zr); break;
    case B::Plus: got = static_cast<bool>(state.plus); break;
    case B::Minus: got = static_cast<bool>(state.minus); break;
    case B::DLeft: got = static_cast<bool>(state.left); break;
    case B::DUp: got = static_cast<bool>(state.up); break;
    case B::DRight: got = static_cast<bool>(state.right); break;
    case B::DDown: got = static_cast<bool>(state.down); break;
    case B::SLLeft: got = static_cast<bool>(state.left_sl); break;
    case B::SRLeft: got = static_cast<bool>(state.left_sr); break;
    case B::SLRight: got = static_cast<bool>(state.right_sl); break;
    case B::SRRight: got = static_cast<bool>(state.right_sr); break;
    default: break;
    }
    return got == pressed;
}

QString GateDetail(const QString& gate, int button, std::size_t player) {
    if (gate == QStringLiteral("gamecube_digital_trigger")) {
        return QStringLiteral("Digital ZL and ZR do not reach a GameCube pad. Use input.trigger.");
    }
    if (gate == QStringLiteral("gamecube_l_overwritten")) {
        return QStringLiteral("GameCube publishing replaces L with the analog trigger digital bit.");
    }
    if (gate == QStringLiteral("style_masked")) {
        return QStringLiteral("This controller style does not publish that button or stick.");
    }
    if (gate == QStringLiteral("style_sensor")) {
        return QStringLiteral("This controller style does not publish that motion sensor.");
    }
    if (gate == QStringLiteral("motion_disabled")) {
        return QStringLiteral("Settings motion_enabled is false.");
    }
    if (gate == QStringLiteral("not_gamecube")) {
        return QStringLiteral("Analog triggers are published only while the style is GameCube.");
    }
    if (gate == QStringLiteral("ring_reads_player1")) {
        return QStringLiteral("The Ring-Con HID bus reads player 1 only.");
    }
    if (gate == QStringLiteral("hid_unavailable")) {
        return QStringLiteral("The HID core is not available, so guest delivery cannot be judged.");
    }
    if (gate == QStringLiteral("configuring")) {
        return QStringLiteral("The controller is configuring and suppresses published state.");
    }
    if (gate == QStringLiteral("not_connected")) {
        return QStringLiteral("The emulated controller is not connected, so the pad poll skips it.");
    }
    if (gate == QStringLiteral("style_none")) {
        return QStringLiteral("The controller style is none, so the pad poll skips it.");
    }
    if (gate == QStringLiteral("system_button_player1")) {
        if (player == 8) {
            return QStringLiteral("Capture and home HID services read player 1. The applet poller also ORs handheld, which still does not update those services.");
        }
        return QStringLiteral("Capture and home HID services read player 1 only.");
    }
    if (gate == QStringLiteral("frontend_unchanged")) {
        if (button == Settings::NativeButton::Home || button == Settings::NativeButton::Screenshot) {
            return QStringLiteral("Home and capture are stored only when system buttons are enabled and the virtual device is loaded.");
        }
        return QStringLiteral("The emulated controller did not take this sample. A centered stick or resting trigger will not steal another device, and virtual devices apply only after input is loaded.");
    }
    if (gate == QStringLiteral("touch_unreadable")) {
        return QStringLiteral("Touch slots are internal. The engine drops a 17th finger with no status.");
    }
    return {};
}

QJsonObject Finish(bool accepted, bool observed, bool frontend, const QString& gate,
                   const QStringList& unconfirmed, const QString& detail, QJsonObject extra = {}) {
    QJsonObject result = std::move(extra);
    result.insert(QStringLiteral("accepted"), accepted);
    result.insert(QStringLiteral("observed"), observed);
    result.insert(QStringLiteral("frontend"), frontend);
    result.insert(QStringLiteral("gated"), !gate.isEmpty());
    result.insert(QStringLiteral("gate"), gate.isEmpty() ? QStringLiteral("none") : gate);
    result.insert(QStringLiteral("guest_delivery"), QStringLiteral("not_claimed"));
    result.insert(QStringLiteral("unconfirmed"), QJsonArray::fromStringList(unconfirmed));
    if (!detail.isEmpty()) {
        result.insert(QStringLiteral("detail"), detail);
    }
    return CS::Success(result);
}

QString ConnectionGate(const Core::HID::EmulatedController* controller) {
    if (controller == nullptr) {
        return QStringLiteral("hid_unavailable");
    }
    if (controller->IsConfiguring()) {
        return QStringLiteral("configuring");
    }
    if (!controller->IsConnected()) {
        return QStringLiteral("not_connected");
    }
    if (controller->GetNpadStyleIndex() == Core::HID::NpadStyleIndex::None) {
        return QStringLiteral("style_none");
    }
    return {};
}
} // namespace

ControlInput::ControlInput(InputCommon::InputSubsystem& subsystem_) : subsystem{subsystem_} {
    clock.start();
    timer.setInterval(20);
    connect(&timer, &QTimer::timeout, this, &ControlInput::Expire);
    timer.start();
}

ControlInput::~ControlInput() {
    Reset();
}

void ControlInput::Reset() {
    for (auto& player : buttons) {
        for (auto& expiry : player) {
            if (expiry) expiry = 1;
        }
    }
    for (auto& player : sticks) {
        for (auto& expiry : player) {
            if (expiry) expiry = 1;
        }
    }
    for (auto& player : motions) {
        for (auto& expiry : player) {
            if (expiry) expiry = 1;
        }
    }
    for (auto& player : triggers) {
        for (auto& expiry : player) {
            if (expiry) expiry = 1;
        }
    }
    for (auto& expiry : rings) if (expiry) expiry = 1;
    for (auto& expiry : touches) if (expiry) expiry = 1;
    Expire();
}

void ControlInput::Expire() {
    const auto now = std::max<qint64>(1, clock.elapsed());
    auto* gamepad = subsystem.GetVirtualGamepad();
    for (std::size_t player = 0; player < buttons.size(); ++player) {
        for (std::size_t button = 0; button < buttons[player].size(); ++button) {
            if (buttons[player][button] && buttons[player][button] <= now) {
                gamepad->SetButtonState(player, static_cast<int>(button), false);
                buttons[player][button] = 0;
            }
        }
        for (std::size_t stick = 0; stick < sticks[player].size(); ++stick) {
            if (sticks[player][stick] && sticks[player][stick] <= now) {
                gamepad->SetStickPosition(player, static_cast<int>(stick), 0.0f, 0.0f);
                sticks[player][stick] = 0;
            }
        }
        for (std::size_t sensor = 0; sensor < motions[player].size(); ++sensor) {
            if (motions[player][sensor] && motions[player][sensor] <= now) {
                gamepad->SetMotionSensor(player, static_cast<int>(sensor), 10000, 0, 0, 0, 0, 0, -1);
                motions[player][sensor] = 0;
            }
        }
        for (std::size_t trigger = 0; trigger < triggers[player].size(); ++trigger) {
            if (triggers[player][trigger] && triggers[player][trigger] <= now) {
                gamepad->SetTriggerAxis(player, static_cast<int>(trigger), 0.0f);
                triggers[player][trigger] = 0;
            }
        }
        if (rings[player] && rings[player] <= now) {
            gamepad->SetRingAxis(player, 0.0f);
            rings[player] = 0;
        }
    }
    for (std::size_t finger = 0; finger < touches.size(); ++finger) {
        if (touches[finger] && touches[finger] <= now) {
            subsystem.GetTouchScreen()->TouchReleased(TouchBase + finger);
            touches[finger] = 0;
        }
    }
}

QJsonObject ControlInput::Handle(const QString& method, const QJsonObject& params) {
    if (method == QStringLiteral("input.schema")) {
        return CS::Success(QJsonObject{
            {QStringLiteral("methods"), QJsonArray{QStringLiteral("input.schema"), QStringLiteral("input.reset"), QStringLiteral("input.state"), QStringLiteral("input.button"), QStringLiteral("input.stick"), QStringLiteral("input.motion"), QStringLiteral("input.trigger"), QStringLiteral("input.ring"), QStringLiteral("input.touch")}},
            {QStringLiteral("buttons"), QJsonArray::fromStringList(ButtonNames)},
            {QStringLiteral("players"), QStringLiteral("0..7 player 1..8, 8 handheld, 9 other")},
            {QStringLiteral("button"), QStringLiteral("{player:0,button:a,state:press|release|tap,duration_ms:100,lease_ms:1000}")},
            {QStringLiteral("stick"), QStringLiteral("{player:0,stick:left|right,x:-1..1,y:-1..1,lease_ms:1000}; positive Y is up")},
            {QStringLiteral("motion"), QStringLiteral("{player:0,sensor:left|right|both,gyro:[x,y,z],accel:[x,y,z],delta_us:10000,lease_ms:1000}; default sensor both writes one sample to each IMU. gyro radians/s, accel G, dt microseconds. Player 8 sensor left is also the console IMU.")},
            {QStringLiteral("trigger"), QStringLiteral("{player:0,trigger:left|right,value:0..1,lease_ms:1000}; GameCube analog. Values at or above 0.5 also press the digital bit. Digital zl/zr do not drive GameCube triggers.")},
            {QStringLiteral("ring"), QStringLiteral("{player:0,value:-1..1,lease_ms:1000}; normalized flex. The guest Ring-Con bus reads player 1 only.")},
            {QStringLiteral("touch"), QStringLiteral("{finger:0..15,state:press|move|release,x:0..1,y:0..1,lease_ms:1000}; top-left origin")},
            {QStringLiteral("status"), QStringLiteral("accepted means the engine call returned. observed means engine readback matches. frontend means the emulated controller matches. gated and gate name a known reason the guest will not publish. guest_delivery is always not_claimed because the HID poll happens later. unconfirmed lists guest flags this call cannot see.")},
            {QStringLiteral("lease"), QStringLiteral("Every held input expires after lease_ms (1..10000, default 1000). Renew that input to hold it longer. Tap duration_ms 1..10000. Expiry is serviced every 20 ms on the GUI thread; external watchdog covers a GUI hang.")}});
    }
    if (method == QStringLiteral("input.reset")) {
        Reset();
        bool observed = true;
        for (const auto& player : buttons) {
            for (const auto expiry : player) observed = observed && expiry == 0;
        }
        for (const auto& player : sticks) {
            for (const auto expiry : player) observed = observed && expiry == 0;
        }
        for (const auto& player : motions) {
            for (const auto expiry : player) observed = observed && expiry == 0;
        }
        for (const auto& player : triggers) {
            for (const auto expiry : player) observed = observed && expiry == 0;
        }
        for (const auto expiry : rings) observed = observed && expiry == 0;
        for (const auto expiry : touches) observed = observed && expiry == 0;
        return Finish(true, observed, false, {}, {QStringLiteral("hid_poll")},
                      QStringLiteral("Active leases were released to neutral. Controls this session was not holding are left unchanged."));
    }
    if (method == QStringLiteral("input.state")) {
        Expire();
        QJsonArray players;
        for (std::size_t player = 0; player < buttons.size(); ++player) {
            QJsonArray pressed;
            for (std::size_t button = 0; button < buttons[player].size(); ++button) {
                if (buttons[player][button]) pressed.append(ButtonNames[static_cast<int>(button)]);
            }
            players.append(QJsonObject{{QStringLiteral("player"), static_cast<int>(player)},
                {QStringLiteral("pressed"), pressed},
                {QStringLiteral("left_stick_active"), sticks[player][0] != 0},
                {QStringLiteral("right_stick_active"), sticks[player][1] != 0},
                {QStringLiteral("motion_active"), motions[player][0] != 0 || motions[player][1] != 0},
                {QStringLiteral("motion_left_active"), motions[player][0] != 0},
                {QStringLiteral("motion_right_active"), motions[player][1] != 0},
                {QStringLiteral("trigger_left_active"), triggers[player][0] != 0},
                {QStringLiteral("trigger_right_active"), triggers[player][1] != 0},
                {QStringLiteral("ring_active"), rings[player] != 0}});
        }
        QJsonArray fingers;
        for (std::size_t finger = 0; finger < touches.size(); ++finger) {
            if (touches[finger]) fingers.append(static_cast<int>(finger));
        }
        return CS::Success(QJsonObject{{QStringLiteral("injected"), players}, {QStringLiteral("touches"), fingers},
            {QStringLiteral("bookkeeping_only"), true}, {QStringLiteral("guest_delivery"), QStringLiteral("not_claimed")}});
    }
    const auto player_value = Default(params, QStringLiteral("player"), 0);
    const auto lease = Default(params, QStringLiteral("lease_ms"), 1000);
    if (!Integer(player_value, 0, 9) || !Integer(lease, 1, 10000)) {
        return CS::Error(QStringLiteral("invalid_params"), QStringLiteral("player must be an integer 0..9 and lease_ms 1..10000"));
    }
    const auto player = static_cast<std::size_t>(player_value.toInt());
    const auto deadline = clock.elapsed() + lease.toInt();
    auto* gamepad = subsystem.GetVirtualGamepad();
    const auto id = Identifier(player);
    if (method == QStringLiteral("input.button")) {
        auto name = params.value(QStringLiteral("button")).toString().toLower();
        if (name == QStringLiteral("sl")) name = QStringLiteral("sl_left");
        if (name == QStringLiteral("sr")) name = QStringLiteral("sr_left");
        const int button = static_cast<int>(ButtonNames.indexOf(name));
        const auto state = params.value(QStringLiteral("state")).toString();
        const auto duration = Default(params, QStringLiteral("duration_ms"), 100);
        if (button < 0 || (state != QStringLiteral("press") && state != QStringLiteral("release") && state != QStringLiteral("tap")) || !Integer(duration, 1, 10000)) {
            return CS::Error(QStringLiteral("invalid_params"), QStringLiteral("Use a button from input.schema, state press/release/tap, duration_ms 1..10000"));
        }
        const bool pressed = state != QStringLiteral("release");
        gamepad->SetButtonState(player, button, pressed);
        buttons[player][button] = state == QStringLiteral("release") ? 0 : state == QStringLiteral("tap") ? clock.elapsed() + duration.toInt() : deadline;
        const bool observed = gamepad->GetButton(id, button) == pressed;
        const auto* controller = ControllerFor(player);
        QString gate;
        bool frontend = false;
        QStringList unconfirmed{QStringLiteral("hid_poll"), QStringLiteral("enable_pad_input"), QStringLiteral("npad_active")};
        if (button == Settings::NativeButton::Home || button == Settings::NativeButton::Screenshot) {
            unconfirmed = {QStringLiteral("hid_poll"), QStringLiteral("system_button_service")};
            if (controller == nullptr) {
                gate = QStringLiteral("hid_unavailable");
            } else if (controller->IsConfiguring()) {
                gate = QStringLiteral("configuring");
            } else {
                frontend = ButtonFrontend(controller, button, pressed);
                if (player != 0) {
                    gate = QStringLiteral("system_button_player1");
                } else if (!frontend) {
                    gate = QStringLiteral("frontend_unchanged");
                }
            }
        } else {
            gate = ConnectionGate(controller);
            if (gate.isEmpty()) {
                frontend = ButtonFrontend(controller, button, pressed);
                gate = ButtonStyleGate(controller->GetNpadStyleIndex(), button);
                if (gate.isEmpty() && !frontend) {
                    gate = QStringLiteral("frontend_unchanged");
                }
            }
        }
        QJsonObject extra;
        if (controller && controller->GetNpadStyleIndex() == Core::HID::NpadStyleIndex::GameCube &&
            button == Settings::NativeButton::R) {
            extra.insert(QStringLiteral("guest_button"), QStringLiteral("zr"));
        }
        return Finish(true, observed, frontend, gate, unconfirmed, GateDetail(gate, button, player), extra);
    }
    if (method == QStringLiteral("input.stick")) {
        const auto stick = params.value(QStringLiteral("stick")).toString();
        if ((stick != QStringLiteral("left") && stick != QStringLiteral("right")) ||
            !Number(params.value(QStringLiteral("x")), -1, 1) || !Number(params.value(QStringLiteral("y")), -1, 1)) {
            return CS::Error(QStringLiteral("invalid_params"), QStringLiteral("stick must be left/right and x/y finite numbers in [-1,1]"));
        }
        const int index = stick == QStringLiteral("left") ? 0 : 1;
        const float x = static_cast<float>(params.value(QStringLiteral("x")).toDouble());
        const float y = static_cast<float>(params.value(QStringLiteral("y")).toDouble());
        gamepad->SetStickPosition(player, index, x, y);
        sticks[player][index] = deadline;
        const bool observed = Same(gamepad->GetAxis(id, index * 2), x) && Same(gamepad->GetAxis(id, index * 2 + 1), y);
        const auto* controller = ControllerFor(player);
        QString gate = ConnectionGate(controller);
        bool frontend = false;
        if (gate.isEmpty()) {
            const auto sticks_now = controller->GetSticks();
            const auto& side = index == 0 ? sticks_now.left : sticks_now.right;
            frontend = NearStick(side.x, x) && NearStick(side.y, y);
            const auto style = controller->GetNpadStyleIndex();
            if ((style == Core::HID::NpadStyleIndex::JoyconLeft && index == 1) ||
                (style == Core::HID::NpadStyleIndex::JoyconRight && index == 0)) {
                gate = QStringLiteral("style_masked");
            } else if (!frontend) {
                gate = QStringLiteral("frontend_unchanged");
            }
        }
        return Finish(true, observed, frontend, gate,
                      {QStringLiteral("hid_poll"), QStringLiteral("enable_pad_input"), QStringLiteral("npad_active")},
                      GateDetail(gate, -1, player));
    }
    if (method == QStringLiteral("input.motion")) {
        const auto gyro = params.value(QStringLiteral("gyro")).toArray();
        const auto accel = params.value(QStringLiteral("accel")).toArray();
        const auto delta = Default(params, QStringLiteral("delta_us"), 10000);
        QString sensor_name = QStringLiteral("both");
        if (params.contains(QStringLiteral("sensor"))) {
            sensor_name = params.value(QStringLiteral("sensor")).toString();
        }
        if (gyro.size() != 3 || accel.size() != 3 || !Integer(delta, 1, 100000) ||
            (sensor_name != QStringLiteral("left") && sensor_name != QStringLiteral("right") && sensor_name != QStringLiteral("both"))) {
            return CS::Error(QStringLiteral("invalid_params"), QStringLiteral("gyro/accel need three components, delta_us must be 1..100000, sensor is left, right, or both"));
        }
        for (int axis = 0; axis < 3; ++axis) {
            if (!Number(gyro[axis], -5, 5) || !Number(accel[axis], -7, 7)) {
                return CS::Error(QStringLiteral("invalid_params"), QStringLiteral("gyro range is +/-5 radians/s; accel range is +/-7 G"));
            }
        }
        const float values[6] = {
            static_cast<float>(gyro[0].toDouble()), static_cast<float>(gyro[1].toDouble()), static_cast<float>(gyro[2].toDouble()),
            static_cast<float>(accel[0].toDouble()), static_cast<float>(accel[1].toDouble()), static_cast<float>(accel[2].toDouble())};
        const int first = sensor_name == QStringLiteral("right") ? 1 : 0;
        const int last = sensor_name == QStringLiteral("left") ? 0 : 1;
        if (sensor_name == QStringLiteral("both")) {
            gamepad->SetMotionState(player, static_cast<u64>(delta.toInt()), values[0], values[1], values[2], values[3], values[4], values[5]);
        }
        const auto* controller = ControllerFor(player);
        const bool motion_enabled = Settings::values.motion_enabled.GetValue();
        QJsonArray sensors;
        QStringList gates;
        bool observed_all = true;
        bool frontend_all = true;
        bool console = false;
        for (int sensor = first; sensor <= last; ++sensor) {
            if (sensor_name != QStringLiteral("both")) {
                gamepad->SetMotionSensor(player, sensor, static_cast<u64>(delta.toInt()), values[0], values[1], values[2], values[3], values[4], values[5]);
            }
            motions[player][static_cast<std::size_t>(sensor)] = deadline;
            const auto motion = gamepad->GetMotion(id, sensor);
            const bool observed = Same(motion.gyro_x, values[0]) && Same(motion.gyro_y, values[1]) && Same(motion.gyro_z, values[2]) &&
                                  Same(motion.accel_x, values[3]) && Same(motion.accel_y, values[4]) && Same(motion.accel_z, values[5]);
            QString gate = ConnectionGate(controller);
            // Configuring does not suppress GetMotions. Keep the connection and style gates.
            if (gate == QStringLiteral("configuring")) {
                gate.clear();
                if (controller && !controller->IsConnected()) {
                    gate = QStringLiteral("not_connected");
                } else if (controller && controller->GetNpadStyleIndex() == Core::HID::NpadStyleIndex::None) {
                    gate = QStringLiteral("style_none");
                }
            }
            bool frontend = false;
            if (controller && gate != QStringLiteral("hid_unavailable")) {
                const auto sample = controller->GetMotions()[static_cast<std::size_t>(sensor)];
                frontend = Near(sample.gyro[0], values[0]) && Near(sample.gyro[1], values[1]) && Near(sample.gyro[2], values[2]) &&
                           Near(sample.accel[0], values[3]) && Near(sample.accel[1], values[4]) && Near(sample.accel[2], values[5]);
            }
            if (gate.isEmpty() && !motion_enabled) {
                gate = QStringLiteral("motion_disabled");
            } else if (gate.isEmpty() && controller && !MotionPublished(controller->GetNpadStyleIndex(), sensor)) {
                gate = QStringLiteral("style_sensor");
            } else if (gate.isEmpty() && !frontend) {
                gate = QStringLiteral("frontend_unchanged");
            }
            if (!gate.isEmpty() && !gates.contains(gate)) {
                gates.append(gate);
            }
            observed_all = observed_all && observed;
            frontend_all = frontend_all && frontend;
            const bool aliases_console = player == 8 && sensor == 0;
            console = console || aliases_console;
            sensors.append(QJsonObject{{QStringLiteral("sensor"), sensor == 0 ? QStringLiteral("left") : QStringLiteral("right")},
                {QStringLiteral("observed"), observed}, {QStringLiteral("frontend"), frontend},
                {QStringLiteral("gate"), gate.isEmpty() ? QStringLiteral("none") : gate},
                {QStringLiteral("console_imu"), aliases_console}});
        }
        QStringList details;
        if (console) {
            details.append(QStringLiteral("Player 8 sensor left is also the console IMU."));
        }
        if (!gates.isEmpty()) {
            const auto gate_detail = GateDetail(gates.front(), -1, player);
            if (!gate_detail.isEmpty()) {
                details.append(gate_detail);
            }
        }
        const auto detail = details.join(QStringLiteral(" "));
        return Finish(true, observed_all, frontend_all, gates.join(QStringLiteral(",")),
                      {QStringLiteral("hid_poll"), QStringLiteral("enable_six_axis_sensor"), QStringLiteral("sixaxis_sensor_enabled")},
                      detail, QJsonObject{{QStringLiteral("sensors"), sensors}, {QStringLiteral("console_imu"), console}});
    }
    if (method == QStringLiteral("input.trigger")) {
        const auto trigger_name = params.value(QStringLiteral("trigger")).toString();
        if ((trigger_name != QStringLiteral("left") && trigger_name != QStringLiteral("right")) ||
            !Number(params.value(QStringLiteral("value")), 0, 1)) {
            return CS::Error(QStringLiteral("invalid_params"), QStringLiteral("trigger must be left/right and value a finite number in [0,1]"));
        }
        const int index = trigger_name == QStringLiteral("left") ? 0 : 1;
        const float value = static_cast<float>(params.value(QStringLiteral("value")).toDouble());
        gamepad->SetTriggerAxis(player, index, value);
        triggers[player][static_cast<std::size_t>(index)] = value > 0.0f ? deadline : 0;
        const bool digital = value >= TriggerPress;
        const bool observed = Same(gamepad->GetAxis(id, TriggerAxisBase + index), value) &&
                              gamepad->GetButton(id, TriggerButtonBase + index) == digital;
        const auto* controller = ControllerFor(player);
        QString gate = ConnectionGate(controller);
        bool frontend = false;
        if (gate.isEmpty() && controller->GetNpadStyleIndex() != Core::HID::NpadStyleIndex::GameCube) {
            gate = QStringLiteral("not_gamecube");
        } else if (gate.isEmpty()) {
            const auto triggers_now = controller->GetTriggers();
            const s32 got = index == 0 ? triggers_now.left : triggers_now.right;
            const auto npad_buttons = controller->GetNpadButtons();
            const bool bit = index == 0 ? static_cast<bool>(npad_buttons.zl) : static_cast<bool>(npad_buttons.zr);
            frontend = NearStick(got, value) && bit == digital;
            if (!frontend) {
                gate = QStringLiteral("frontend_unchanged");
            }
        }
        return Finish(true, observed, frontend, gate,
                      {QStringLiteral("hid_poll"), QStringLiteral("enable_pad_input"), QStringLiteral("npad_active")},
                      GateDetail(gate, -1, player));
    }
    if (method == QStringLiteral("input.ring")) {
        if (!Number(params.value(QStringLiteral("value")), -1, 1)) {
            return CS::Error(QStringLiteral("invalid_params"), QStringLiteral("value must be a finite number in [-1,1]"));
        }
        const float value = static_cast<float>(params.value(QStringLiteral("value")).toDouble());
        gamepad->SetRingAxis(player, value);
        rings[player] = value != 0.0f ? deadline : 0;
        const bool observed = Same(gamepad->GetAxis(id, RingAxisX), value) && Same(gamepad->GetAxis(id, RingAxisX + 1), 0.0f);
        const auto* controller = ControllerFor(player);
        QString gate;
        bool frontend = false;
        if (controller == nullptr) {
            gate = QStringLiteral("hid_unavailable");
        } else if (controller->IsConfiguring()) {
            gate = QStringLiteral("configuring");
        } else {
            frontend = Near(controller->GetRingSensorForce().force, value);
            if (player != 0) {
                gate = QStringLiteral("ring_reads_player1");
            } else if (!frontend) {
                gate = QStringLiteral("frontend_unchanged");
            }
        }
        return Finish(true, observed, frontend, gate,
                      {QStringLiteral("hid_poll"), QStringLiteral("ring_device_enabled")},
                      GateDetail(gate, -1, player));
    }
    if (method == QStringLiteral("input.touch")) {
        const auto finger_value = params.value(QStringLiteral("finger"));
        const auto state = params.value(QStringLiteral("state")).toString();
        if (!Integer(finger_value, 0, 15) || (state != QStringLiteral("press") && state != QStringLiteral("move") && state != QStringLiteral("release"))) {
            return CS::Error(QStringLiteral("invalid_params"), QStringLiteral("finger must be 0..15 and state press/move/release"));
        }
        const auto finger = static_cast<std::size_t>(finger_value.toInt());
        if (state == QStringLiteral("release")) {
            subsystem.GetTouchScreen()->TouchReleased(TouchBase + finger);
            touches[finger] = 0;
        } else {
            if (!Number(params.value(QStringLiteral("x")), 0, 1) || !Number(params.value(QStringLiteral("y")), 0, 1)) {
                return CS::Error(QStringLiteral("invalid_params"), QStringLiteral("x/y must be finite normalized coordinates in [0,1]"));
            }
            subsystem.GetTouchScreen()->TouchMoved(static_cast<float>(params.value(QStringLiteral("x")).toDouble()), static_cast<float>(params.value(QStringLiteral("y")).toDouble()), TouchBase + finger);
            touches[finger] = deadline;
        }
        return Finish(true, false, false, {},
                      {QStringLiteral("hid_poll"), QStringLiteral("touch_finger_unreadable"), QStringLiteral("seventeenth_finger_dropped_silently")},
                      GateDetail(QStringLiteral("touch_unreadable"), -1, player));
    }
    return CS::Error(QStringLiteral("unknown_method"), QStringLiteral("Unknown input method; inspect input.schema"));
}
