// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <functional>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QObject>

class ControlInput;
class ControlServer;
class MainWindow;

// Keeps emulator-specific commands outside the generic Qt inspection server.
class ControlAdapter final : public QObject {
public:
    ControlAdapter(MainWindow& window, ControlServer& server);
    ~ControlAdapter() override;
    QJsonObject Handle(const QString& method, const QJsonObject& params);

private:
    QJsonObject QueueGame(const QString& method, std::function<QJsonObject()> operation);
    MainWindow& window;
    ControlServer& server;
    std::unique_ptr<ControlInput> input;
    QElapsedTimer telemetry_clock;
    qint64 last_stats_ms{-1};
    qint64 last_frame_ms{-1};
    double game_fps{};
    double system_fps{};
    int shaders_building{};
    bool game_job_pending{};
};
