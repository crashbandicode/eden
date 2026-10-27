// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace Tegra {

// Set once a Switch 2 (Ampere, NVN2) driver is talking to the GPU. Its command streams use
// Ampere classes and the Turing+ macro ISA, which Eden's Maxwell engines don't implement.
inline std::atomic<bool> g_nx2_ampere{false};

// NVN2 queues release their progress values into one GPU page and wait (GPU acquires and CPU
// polls) on the same offsets in the page below it. Something on the console copies one into
// the other; until that is known, the puller mirrors releases that land in the source page.
inline std::atomic<u64> g_nx2_semaphore_source_page{0}; // GPU VA of the released-to page
inline std::atomic<u64> g_nx2_semaphore_mirror_page{0}; // GPU VA of the waited-on page

} // namespace Tegra
