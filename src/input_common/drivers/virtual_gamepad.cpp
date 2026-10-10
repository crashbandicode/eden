// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>

#include "input_common/drivers/virtual_gamepad.h"

namespace InputCommon {
constexpr std::size_t PlayerIndexCount = 10;
// NativeButton::NumButtons. Ids 0..19 match the Android overlay; 20 and 21 are right SL/SR.
constexpr int ButtonCount = 22;
constexpr int StickCount = 2;
constexpr int MotionCount = 2;
// Keep these ids aligned with EmulatedController::LoadVirtualGamepadParams.
constexpr int TriggerCount = 2;
constexpr int TriggerButtonBase = 32;
constexpr int TriggerAxisBase = 6;
constexpr int RingAxisX = 4;
constexpr int RingAxisY = 5;
constexpr float TriggerPress = 0.5f;
constexpr u64 RestDeltaUs = 10000;

VirtualGamepad::VirtualGamepad(std::string input_engine_) : InputEngine(std::move(input_engine_)) {
    for (std::size_t i = 0; i < PlayerIndexCount; i++) {
        const auto identifier = GetIdentifier(i);
        PreSetController(identifier);
        for (int button = 0; button < ButtonCount; ++button) {
            PreSetButton(identifier, button);
        }
        for (int trigger = 0; trigger < TriggerCount; ++trigger) {
            PreSetButton(identifier, TriggerButtonBase + trigger);
            PreSetAxis(identifier, TriggerAxisBase + trigger);
        }
        for (int axis = 0; axis < StickCount * 2; ++axis) {
            PreSetAxis(identifier, axis);
        }
        PreSetAxis(identifier, RingAxisX);
        PreSetAxis(identifier, RingAxisY);
        for (int sensor = 0; sensor < MotionCount; ++sensor) {
            PreSetMotion(identifier, sensor);
        }
    }
}

void VirtualGamepad::SetButtonState(std::size_t player_index, int button_id, bool value) {
    if (player_index >= PlayerIndexCount || button_id < 0 || button_id >= ButtonCount) {
        return;
    }
    SetButton(GetIdentifier(player_index), button_id, value);
}

void VirtualGamepad::SetButtonState(std::size_t player_index, VirtualButton button_id, bool value) {
    SetButtonState(player_index, static_cast<int>(button_id), value);
}

void VirtualGamepad::SetStickPosition(std::size_t player_index, int axis_id, float x_value,
                                      float y_value) {
    if (player_index >= PlayerIndexCount || axis_id < 0 || axis_id >= StickCount) {
        return;
    }
    const auto identifier = GetIdentifier(player_index);
    SetAxis(identifier, axis_id * 2, x_value);
    SetAxis(identifier, (axis_id * 2) + 1, y_value);
}

void VirtualGamepad::SetStickPosition(std::size_t player_index, VirtualStick axis_id, float x_value,
                                      float y_value) {
    SetStickPosition(player_index, static_cast<int>(axis_id), x_value, y_value);
}

void VirtualGamepad::SetMotionState(std::size_t player_index, u64 delta_timestamp, float gyro_x,
                                    float gyro_y, float gyro_z, float accel_x, float accel_y,
                                    float accel_z) {
    for (int sensor = 0; sensor < MotionCount; ++sensor) {
        SetMotionSensor(player_index, sensor, delta_timestamp, gyro_x, gyro_y, gyro_z, accel_x,
                        accel_y, accel_z);
    }
}

void VirtualGamepad::SetMotionSensor(std::size_t player_index, int sensor, u64 delta_timestamp,
                                     float gyro_x, float gyro_y, float gyro_z, float accel_x,
                                     float accel_y, float accel_z) {
    if (player_index >= PlayerIndexCount || sensor < 0 || sensor >= MotionCount) {
        return;
    }
    const BasicMotion motion_data{
        .gyro_x = gyro_x,
        .gyro_y = gyro_y,
        .gyro_z = gyro_z,
        .accel_x = accel_x,
        .accel_y = accel_y,
        .accel_z = accel_z,
        .delta_timestamp = delta_timestamp,
    };
    SetMotion(GetIdentifier(player_index), sensor, motion_data);
}

void VirtualGamepad::SetTriggerAxis(std::size_t player_index, int trigger, float value) {
    if (player_index >= PlayerIndexCount || trigger < 0 || trigger >= TriggerCount ||
        !std::isfinite(value)) {
        return;
    }
    value = std::clamp(value, 0.0f, 1.0f);
    const auto identifier = GetIdentifier(player_index);
    SetAxis(identifier, TriggerAxisBase + trigger, value);
    SetButton(identifier, TriggerButtonBase + trigger, value >= TriggerPress);
}

void VirtualGamepad::SetRingAxis(std::size_t player_index, float value) {
    if (player_index >= PlayerIndexCount || !std::isfinite(value)) {
        return;
    }
    value = std::clamp(value, -1.0f, 1.0f);
    const auto identifier = GetIdentifier(player_index);
    SetAxis(identifier, RingAxisX, value);
    SetAxis(identifier, RingAxisY, 0.0f);
}

void VirtualGamepad::ResetControllers() {
    for (std::size_t i = 0; i < PlayerIndexCount; i++) {
        SetStickPosition(i, VirtualStick::Left, 0.0f, 0.0f);
        SetStickPosition(i, VirtualStick::Right, 0.0f, 0.0f);
        for (int button = 0; button < ButtonCount; ++button) {
            SetButtonState(i, button, false);
        }
        for (int trigger = 0; trigger < TriggerCount; ++trigger) {
            SetTriggerAxis(i, trigger, 0.0f);
        }
        SetRingAxis(i, 0.0f);
        for (int sensor = 0; sensor < MotionCount; ++sensor) {
            SetMotionSensor(i, sensor, RestDeltaUs, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f);
        }
    }
}

PadIdentifier VirtualGamepad::GetIdentifier(std::size_t player_index) const {
    return {
        .guid = Common::UUID{},
        .port = player_index,
        .pad = 0,
    };
}

} // namespace InputCommon
