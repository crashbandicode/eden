// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

namespace InputCommon { class InputSubsystem; }

class ControlInput final : public QObject {
public:
    explicit ControlInput(InputCommon::InputSubsystem& subsystem);
    ~ControlInput() override;
    QJsonObject Handle(const QString& method, const QJsonObject& params);
    void Reset();

private:
    void Expire();
    InputCommon::InputSubsystem& subsystem;
    QElapsedTimer clock;
    QTimer timer;
    std::array<std::array<qint64, 22>, 10> buttons{};
    std::array<std::array<qint64, 2>, 10> sticks{};
    std::array<std::array<qint64, 2>, 10> motions{};
    std::array<std::array<qint64, 2>, 10> triggers{};
    std::array<qint64, 10> rings{};
    std::array<qint64, 16> touches{};
};
