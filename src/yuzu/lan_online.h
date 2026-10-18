// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include "common/common_types.h"

class QWidget;

namespace LanOnline {
// Called on the UI thread, before guest threads start. Empty string means ready.
QString Prepare(QWidget* parent, u64 title_id);
} // namespace LanOnline
