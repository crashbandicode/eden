// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "common/common_types.h"
#include "common/socket_types.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

namespace Network {

/// Datagram framing shared with the private LAN broker. Every datagram exchanged over a guest
/// socket's Unix datagram descriptor starts with this fixed big-endian header:
///   0..3 "ELN1", 4 type (1 = data), 5..7 zero, 8..11 source IPv4, 12..15 destination IPv4,
///   16..17 source port, 18..19 destination port, 20..23 payload length; payload follows.
namespace NsoLanWire {

inline constexpr std::size_t HeaderSize = 24;
/// The 24-byte envelope plus the payload must fit the largest IPv4 UDP payload (65507), because
/// the broker may carry the very same frame in one UDP datagram on the LAN.
inline constexpr std::size_t MaxPayloadSize = 65483;
static_assert(HeaderSize + MaxPayloadSize == 65507);
inline constexpr std::array<u8, 4> Magic{'E', 'L', 'N', '1'};
inline constexpr u8 TypeData = 1;

struct Header {
    IPv4Address source;
    IPv4Address destination;
    u16 source_port;
    u16 destination_port;
    u32 payload_size;
};

enum class DecodeStatus {
    Ok,
    TooShort,
    BadMagic,
    BadType,
    BadReserved,
    PayloadTooLarge,
    LengthMismatch,
};

[[nodiscard]] std::string_view ToString(DecodeStatus status);

/// Serializes `header` exactly as it appears on the wire.
[[nodiscard]] std::array<u8, HeaderSize> EncodeHeader(const Header& header);

/// Validates magic, type, reserved bytes and that `datagram` is exactly header + payload_size
/// bytes with payload_size <= MaxPayloadSize. Fills `header` only on Ok.
[[nodiscard]] DecodeStatus DecodeHeader(std::span<const u8> datagram, Header& header);

/// 10.253.0.0/16 excluding the network (.0.0) and broadcast (.255.255) addresses.
[[nodiscard]] bool IsPrivateGuestAddress(const IPv4Address& address);

/// A guest address or one of the two broker services (10.254.0.1, 10.254.0.2).
[[nodiscard]] bool IsAllowedPeerAddress(const IPv4Address& address);

} // namespace NsoLanWire

/// UDP guest socket carried over a real AF_UNIX/SOCK_DGRAM descriptor to a local LAN broker.
///
/// The guest only ever sees its virtual IPv4 address and guest port. The broker, reached through
/// a newline-delimited JSON control socket, maps (virtual address, guest port) to the Unix
/// pathname this socket owns and moves datagrams between instances. Only INET + DGRAM +
/// UDP/IP/unspecified is supported. Broadcast and multicast are rejected rather than emulated.
///
/// The descriptor is real, so non-blocking mode, receive/send timeouts and Network::Poll work
/// on it directly. Linux only.
class NsoLanSocket final : public Socket {
public:
    NsoLanSocket(std::string control_path, std::string datagram_path, IPv4Address virtual_address);
    ~NsoLanSocket() override;

    NsoLanSocket(const NsoLanSocket&) = delete;
    NsoLanSocket& operator=(const NsoLanSocket&) = delete;

    Errno Initialize(Domain domain, Type type, Protocol protocol) override;
    Errno Close() override;
    std::pair<AcceptResult, Errno> Accept() override;
    Errno Connect(SockAddrIn addr_in) override;
    std::pair<SockAddrIn, Errno> GetPeerName() override;
    std::pair<SockAddrIn, Errno> GetSockName() override;
    Errno Bind(SockAddrIn addr) override;
    Errno Listen(s32 backlog) override;
    Errno Shutdown(ShutdownHow how) override;
    std::pair<s32, Errno> Recv(int flags, std::span<u8> message) override;
    std::pair<s32, Errno> RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) override;
    std::pair<s32, Errno> Send(std::span<const u8> message, int flags) override;
    std::pair<s32, Errno> SendTo(u32 flags, std::span<const u8> message,
                                 const SockAddrIn* addr) override;

    /// Guest timeouts are milliseconds; zero disables the timeout.
    Errno SetSndTimeo(u32 value) override;
    Errno SetRcvTimeo(u32 value) override;

private:
    Errno SetTimeout(int option, u32 milliseconds);
    Errno RegisterLocked(u16 requested_port);
    Errno AutoBindLocked();
    void UnregisterLocked();
    void CloseLocked();
    void RemoveSocketFileLocked();
    std::pair<s32, Errno> SendDatagram(const SockAddrIn& destination,
                                       std::span<const u8> message, u32 flags);
    std::pair<s32, Errno> ReceiveDatagram(u32 flags, std::span<u8> message, SockAddrIn* addr);

    const std::string control_path;
    const std::string datagram_path;
    const IPv4Address virtual_address;

    std::mutex state_mutex;
    std::string local_path;  ///< Unix pathname this socket created; empty when not created.
    u64 local_device = 0;    ///< Identity of the node we created, checked before unlinking.
    u64 local_inode = 0;
    u16 bound_port = 0;      ///< Guest port registered with the broker; 0 when unbound.
    std::optional<SockAddrIn> peer;
    bool shutdown_read = false;
    bool shutdown_write = false;
};

} // namespace Network
