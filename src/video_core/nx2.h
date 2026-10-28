// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>

#include "common/common_types.h"

namespace Tegra {

// Set once a Switch 2 (Ampere, NVN2) driver is talking to the GPU. Its command streams use
// Ampere classes and the Turing+ macro ISA, which Eden's Maxwell engines don't implement.
inline std::atomic<bool> g_nx2_ampere{false};

// NVN2 queues release their progress values into the second page of a two-page GPU-accessible
// region and GPU acquires read the same offsets in the first page (the CPU polls the second).
// Something on the console copies one into the other; until that is known, the puller mirrors
// releases that land in a second page. Every queue has such a region, so keep a small table.
inline constexpr size_t Nx2MaxSemaphoreRegions = 32;
inline std::array<std::atomic<u64>, Nx2MaxSemaphoreRegions> g_nx2_semaphore_regions{}; // starts

inline void Nx2AddSemaphoreRegion(u64 start) {
    for (auto& region : g_nx2_semaphore_regions) {
        if (region.load() == start) {
            return;
        }
    }
    for (auto& region : g_nx2_semaphore_regions) {
        u64 expected = 0;
        if (region.compare_exchange_strong(expected, start)) {
            return;
        }
    }
}

/// The region's memory was destroyed: stop mirroring into it.
inline void Nx2RemoveSemaphoreRegion(u64 start) {
    for (auto& region : g_nx2_semaphore_regions) {
        u64 expected = start;
        region.compare_exchange_strong(expected, 0);
    }
}

/// GPU VA in the first page that mirrors a release at `address`, or 0.
inline u64 Nx2SemaphoreMirror(u64 address) {
    for (const auto& region : g_nx2_semaphore_regions) {
        const u64 start = region.load(std::memory_order_relaxed);
        if (start == 0) {
            continue;
        }
        if (address >= start + 0x1000 && address < start + 0x2000) {
            return address - 0x1000;
        }
    }
    return 0;
}

} // namespace Tegra
