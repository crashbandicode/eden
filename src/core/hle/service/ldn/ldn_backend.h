// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace Service::LDN {
enum class SocketBackend { Host, Room, AutomaticLanOnline };
// Explicit LDN LAN uses the selected host interface even if a frontend room is joined.
constexpr SocketBackend SelectSocketBackend(bool mitm, bool automatic, bool room, bool datagram) {
    if (mitm) { return SocketBackend::Host; }
    if (automatic) {
        return datagram ? SocketBackend::AutomaticLanOnline : SocketBackend::Host;
    }
    return room ? SocketBackend::Room : SocketBackend::Host;
}
} // namespace Service::LDN
