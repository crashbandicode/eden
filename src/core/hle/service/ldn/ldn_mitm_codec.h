// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Service::LDN::Mitm {

enum class PacketType : std::uint8_t { Scan = 0, ScanResp = 1, Connect = 2, SyncNetwork = 3 };
inline constexpr std::size_t HeaderSize = 12;
inline constexpr std::size_t FrameSizeMax = 2048;
inline constexpr std::size_t PayloadSizeMax = 0x480;

struct Header {
    PacketType type;
    bool compressed;
    std::uint16_t encoded_size;
    std::uint16_t decoded_size;
};

struct Message {
    PacketType type;
    std::vector<std::uint8_t> payload;
};

// DecodeHeader accepts a header prefix; Decode requires exactly one whole datagram.
std::optional<Header> DecodeHeader(std::span<const std::uint8_t> bytes, std::string& error);
std::optional<Message> Decode(std::span<const std::uint8_t> frame, std::string& error);
std::optional<std::vector<std::uint8_t>> Encode(PacketType type,
                                              std::span<const std::uint8_t> payload,
                                              std::string& error, bool compress = true);

// One instance belongs to one TCP connection. Valid complete frames preceding a
// malformed frame remain in output; no bytes from the malformed frame are published.
// A failed connection requires Reset before any further input is accepted.
class StreamDecoder {
public:
    bool Feed(std::span<const std::uint8_t> bytes, std::vector<Message>& output,
              std::string& error);
    void Reset();
    std::size_t BufferedSize() const;
    bool Failed() const;

private:
    std::array<std::uint8_t, FrameSizeMax> buffer{};
    std::size_t buffered{};
    std::optional<Header> header;
    bool failed{};
};

} // namespace Service::LDN::Mitm
