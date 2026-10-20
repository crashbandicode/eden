// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>

namespace Core {
class System;
}

QJsonObject HandleGuestTracepoint(Core::System& system, const QString& method,
                                 const QJsonObject& params);
