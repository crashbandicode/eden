// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Network {
class SocketBase;
}

namespace Service::Sockets {

/// Guest socket descriptors shared by every bsd service instance and their host threads.
///
/// The mutex guards only the table and is never held while calling into a socket. Callers take
/// a strong snapshot, release the lock and then operate on the snapshot, so a blocking call does
/// not hold the table and a concurrent Close cannot destroy a socket that is still in use.
/// Each publication gets a unique serial; updates and removals that name an older serial are
/// refused, so a Close or Fcntl that raced with descriptor reuse cannot touch the replacement.
class DescriptorTable {
public:
    static constexpr size_t MAX_FD = 128;

    struct Entry {
        std::shared_ptr<Network::SocketBase> socket;
        s32 flags = 0;
        bool is_connection_based = false;
        u64 serial = 0; ///< Assigned by Insert; identifies this publication of the descriptor.
    };

    [[nodiscard]] static constexpr bool InRange(s32 fd) noexcept {
        return fd >= 0 && fd < static_cast<s32>(MAX_FD);
    }

    /// Strong copy of the entry, or nullopt when fd is out of range or not allocated.
    [[nodiscard]] std::optional<Entry> Get(s32 fd) const {
        if (!InRange(fd)) {
            return std::nullopt;
        }
        std::scoped_lock lock{mutex};
        return entries[fd];
    }

    [[nodiscard]] bool HasFree() const {
        std::scoped_lock lock{mutex};
        for (const auto& entry : entries) {
            if (!entry) {
                return true;
            }
        }
        return false;
    }

    /// Publishes the entry at the lowest free descriptor under a new serial.
    /// Returns -1 without publishing when the table is full.
    [[nodiscard]] s32 Insert(Entry entry) {
        std::scoped_lock lock{mutex};
        for (s32 fd = 0; fd < static_cast<s32>(MAX_FD); ++fd) {
            if (!entries[fd]) {
                entry.serial = next_serial++;
                entries[fd] = std::move(entry);
                return fd;
            }
        }
        return -1;
    }

    /// Removes fd only while it still holds the publication identified by serial.
    bool Erase(s32 fd, u64 serial) {
        std::optional<Entry> removed;
        {
            std::scoped_lock lock{mutex};
            if (!IsCurrentLocked(fd, serial)) {
                return false;
            }
            removed = std::exchange(entries[fd], std::nullopt);
        }
        // The table's reference may be the last one; destroy the socket outside the lock.
        return true;
    }

    /// Stores flags only while fd still holds the publication identified by serial.
    bool SetFlags(s32 fd, u64 serial, s32 flags) {
        std::scoped_lock lock{mutex};
        if (!IsCurrentLocked(fd, serial)) {
            return false;
        }
        entries[fd]->flags = flags;
        return true;
    }

    /// Current flags of the publication identified by serial, or nullopt once it was removed.
    [[nodiscard]] std::optional<s32> GetFlags(s32 fd, u64 serial) const {
        std::scoped_lock lock{mutex};
        if (!IsCurrentLocked(fd, serial)) {
            return std::nullopt;
        }
        return entries[fd]->flags;
    }

    /// Strong references to the socket of every allocated descriptor, in descriptor order.
    [[nodiscard]] std::vector<std::shared_ptr<Network::SocketBase>> Sockets() const {
        std::vector<std::shared_ptr<Network::SocketBase>> sockets;
        std::scoped_lock lock{mutex};
        for (const auto& entry : entries) {
            if (entry && entry->socket) {
                sockets.push_back(entry->socket);
            }
        }
        return sockets;
    }

private:
    [[nodiscard]] bool IsCurrentLocked(s32 fd, u64 serial) const noexcept {
        return InRange(fd) && entries[fd] && entries[fd]->serial == serial;
    }

    mutable std::mutex mutex;
    std::array<std::optional<Entry>, MAX_FD> entries{};
    u64 next_serial = 1;
};

} // namespace Service::Sockets
