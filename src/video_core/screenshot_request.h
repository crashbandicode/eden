// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <utility>

namespace VideoCore {

// Multiple capture producers publish one request to the render thread. Its payload must
// be complete before Pending becomes true, and cannot change until Complete releases it.
class ScreenshotRequest {
public:
    bool Pending() const {
        return pending.load(std::memory_order_acquire);
    }

    template <typename Publish>
    bool TryRequest(Publish&& publish) {
        std::scoped_lock lock{producers};
        if (Pending()) {
            return false;
        }
        std::forward<Publish>(publish)();
        pending.store(true, std::memory_order_release);
        return true;
    }

    // Render thread only, after consuming all payload fields and handing off the callback.
    void Complete() {
        pending.store(false, std::memory_order_release);
    }

private:
    std::mutex producers;
    std::atomic<bool> pending{false};
};

} // namespace VideoCore
