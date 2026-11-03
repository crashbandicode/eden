// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFile>
#include <QJsonObject>
#include <QObject>
#include "common/common_types.h"

namespace Core { class System; }
namespace Kernel { class KProcess; }

class ControlTrace final : public QObject {
public:
    explicit ControlTrace(Core::System& system, QObject* parent = nullptr);
    ~ControlTrace() override;
    QJsonObject Handle(const QString& method, const QJsonObject& params);
    bool LoadConfig(const QString& path);
    QString StartupError() const { return startup_error; }
    void Drain();
    bool Flush();
    bool ValidateBoot();
    void BeforeStop() { process_ready = false; }
    void AfterStop() { process_ready = false; Flush(); }
private:
    QJsonObject Configure(const QJsonObject& params);
    void Refresh(Kernel::KProcess* process);
    Core::System& system;
    QFile output;
    u64 cursor{};
    u64 file_lost{};
    QString io_error;
    QString startup_error;
    bool process_ready{}; // GUI-thread lifecycle; core lifetime lock covers async exits.
};
