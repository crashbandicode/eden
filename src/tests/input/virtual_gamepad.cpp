// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/settings_input.h"
#include "input_common/drivers/virtual_gamepad.h"

namespace {
using Pad = InputCommon::VirtualGamepad;

static_assert(static_cast<int>(Pad::VirtualButton::ButtonA) == Settings::NativeButton::A);
static_assert(static_cast<int>(Pad::VirtualButton::ButtonCapture) == Settings::NativeButton::Screenshot);
static_assert(static_cast<int>(Pad::VirtualButton::ButtonSLRight) == Settings::NativeButton::SLRight);
static_assert(static_cast<int>(Pad::VirtualButton::ButtonSRRight) == Settings::NativeButton::SRRight);
static_assert(Settings::NativeButton::NumButtons == 22);

PadIdentifier Player(std::size_t port) {
    return {.guid = Common::UUID{}, .port = port, .pad = 0};
}
} // namespace

TEST_CASE("virtual gamepad ignores an out of range player", "[input]") {
    Pad pad("virtual_gamepad");
    REQUIRE_NOTHROW(pad.SetButtonState(10, 0, true));
    REQUIRE_NOTHROW(pad.SetStickPosition(10, 0, 1.0f, 1.0f));
    REQUIRE_NOTHROW(pad.SetMotionState(10, 10000, 0.25f, 0, 0, 0, 0, -1.0f));
    REQUIRE_NOTHROW(pad.SetMotionSensor(10, 0, 10000, 0.25f, 0, 0, 0, 0, -1.0f));
    REQUIRE_NOTHROW(pad.SetTriggerAxis(10, 0, 1.0f));
    REQUIRE_NOTHROW(pad.SetRingAxis(10, 1.0f));
    REQUIRE_FALSE(pad.GetButton(Player(10), 0));
    REQUIRE_FALSE(pad.GetButton(Player(0), 0));
}

TEST_CASE("virtual gamepad rejects a negative button and a third stick", "[input]") {
    Pad pad("virtual_gamepad");
    const auto id = Player(0);
    REQUIRE_NOTHROW(pad.SetButtonState(0, -1, true));
    REQUIRE_FALSE(pad.GetButton(id, 0));
    pad.SetRingAxis(0, 0.5f);
    REQUIRE_NOTHROW(pad.SetStickPosition(0, 2, 0.0f, 0.0f));
    REQUIRE(pad.GetAxis(id, 4) == 0.5f);
    REQUIRE(pad.GetAxis(id, 5) == 0.0f);
}

TEST_CASE("virtual gamepad sets and resets right SL and SR", "[input]") {
    Pad pad("virtual_gamepad");
    const auto id = Player(1);
    pad.SetButtonState(1, Pad::VirtualButton::ButtonSLRight, true);
    pad.SetButtonState(1, Pad::VirtualButton::ButtonSRRight, true);
    REQUIRE(pad.GetButton(id, 20));
    REQUIRE(pad.GetButton(id, 21));
    REQUIRE_FALSE(pad.GetButton(id, 16));
    pad.ResetControllers();
    REQUIRE_FALSE(pad.GetButton(id, 20));
    REQUIRE_FALSE(pad.GetButton(id, 21));
    REQUIRE_FALSE(pad.GetButton(Player(0), 0));
}

TEST_CASE("virtual gamepad keeps motion sensors independent", "[input]") {
    Pad pad("virtual_gamepad");
    const auto id = Player(8);
    pad.SetMotionState(8, 10000, 0.25f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f);
    REQUIRE(pad.GetMotion(id, 0).gyro_x == 0.25f);
    REQUIRE(pad.GetMotion(id, 1).gyro_x == 0.25f);
    REQUIRE(pad.GetMotion(id, 0).accel_z == -1.0f);
    pad.SetMotionSensor(8, 1, 10000, -0.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f);
    REQUIRE(pad.GetMotion(id, 0).gyro_x == 0.25f);
    REQUIRE(pad.GetMotion(id, 1).gyro_x == -0.5f);
    REQUIRE_NOTHROW(pad.SetMotionSensor(8, 2, 10000, 0.5f, 0, 0, 0, 0, -1));
    REQUIRE(pad.GetMotion(id, 1).gyro_x == -0.5f);
    pad.ResetControllers();
    REQUIRE(pad.GetMotion(id, 0).gyro_x == 0.0f);
    REQUIRE(pad.GetMotion(id, 0).accel_z == -1.0f);
    REQUIRE(pad.GetMotion(id, 1).accel_z == -1.0f);
}

TEST_CASE("virtual gamepad trigger and ring axes match the HID binding", "[input]") {
    Pad pad("virtual_gamepad");
    const auto id = Player(0);
    pad.SetTriggerAxis(0, 0, 0.25f);
    REQUIRE(pad.GetAxis(id, 6) == 0.25f);
    REQUIRE_FALSE(pad.GetButton(id, 32));
    pad.SetTriggerAxis(0, 1, 0.5f);
    REQUIRE(pad.GetAxis(id, 7) == 0.5f);
    REQUIRE(pad.GetButton(id, 33));
    REQUIRE_FALSE(pad.GetButton(id, 32));
    pad.SetRingAxis(0, -0.5f);
    REQUIRE(pad.GetAxis(id, 4) == -0.5f);
    REQUIRE(pad.GetAxis(id, 5) == 0.0f);
    // Sticks stay on axes 0..3.
    pad.SetStickPosition(0, Pad::VirtualStick::Left, 1.0f, -1.0f);
    REQUIRE(pad.GetAxis(id, 0) == 1.0f);
    REQUIRE(pad.GetAxis(id, 1) == -1.0f);
    REQUIRE(pad.GetAxis(id, 4) == -0.5f);
    pad.ResetControllers();
    REQUIRE(pad.GetAxis(id, 6) == 0.0f);
    REQUIRE(pad.GetAxis(id, 7) == 0.0f);
    REQUIRE_FALSE(pad.GetButton(id, 33));
    REQUIRE(pad.GetAxis(id, 4) == 0.0f);
    REQUIRE(pad.GetAxis(id, 0) == 0.0f);
}
