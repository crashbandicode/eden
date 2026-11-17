// SPDX-License-Identifier: GPL-3.0-or-later
// Original implementation from published wire facts. Protocol credit:
// spacemeowx2/ldn_mitm (9a2639eff5b2e60065be3dfae69c46de6c3d7648),
// and Ryujinx contributors (c14ce4d2a5c9b373fb454906a6dc142c028d7be2).
// No implementation bodies are adapted from either reference.

#include "core/hle/service/ldn/ldn_mitm_codec.h"

#include <algorithm>
#include <utility>

namespace Service::LDN::Mitm {
namespace {
std::optional<std::size_t> PayloadSize(PacketType type) {
    switch (type) {
    case PacketType::Scan:
        return 0;
    case PacketType::ScanResp:
    case PacketType::SyncNetwork:
        return PayloadSizeMax;
    case PacketType::Connect:
        return 0x40;
    }
    return std::nullopt;
}

std::uint16_t Read16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(bytes[offset]) |
                                      (static_cast<unsigned>(bytes[offset + 1]) << 8));
}

void Write16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}
} // namespace

std::optional<Header> DecodeHeader(std::span<const std::uint8_t> bytes, std::string& error) {
    error.clear();
    if (bytes.size() < HeaderSize) {
        error = "truncated header";
        return std::nullopt;
    }
    const auto magic = static_cast<std::uint32_t>(bytes[0]) |
                       (static_cast<std::uint32_t>(bytes[1]) << 8) |
                       (static_cast<std::uint32_t>(bytes[2]) << 16) |
                       (static_cast<std::uint32_t>(bytes[3]) << 24);
    if (magic != 0x11451400) {
        error = "invalid magic";
        return std::nullopt;
    }
    const auto type = static_cast<PacketType>(bytes[4]);
    const auto expected = PayloadSize(type);
    if (!expected) {
        error = "unknown packet type";
        return std::nullopt;
    }
    if (bytes[5] > 1) {
        error = "invalid compression flag";
        return std::nullopt;
    }
    const Header header{type, bytes[5] == 1, Read16(bytes, 6), Read16(bytes, 8)};
    if (header.encoded_size > FrameSizeMax - HeaderSize) {
        error = "wire body exceeds frame limit";
        return std::nullopt;
    }
    if ((header.compressed ? header.decoded_size : header.encoded_size) != *expected) {
        error = "payload size disagrees with packet type";
        return std::nullopt;
    }
    return header;
}

std::optional<Message> Decode(std::span<const std::uint8_t> frame, std::string& error) {
    const auto header = DecodeHeader(frame, error);
    if (!header) {
        return std::nullopt;
    }
    if (frame.size() != HeaderSize + header->encoded_size) {
        error = "frame length disagrees with header";
        return std::nullopt;
    }
    const auto body = frame.subspan(HeaderSize);
    Message message{header->type, {}};
    if (!header->compressed) {
        message.payload.assign(body.begin(), body.end());
        return message;
    }
    message.payload.reserve(header->decoded_size);
    for (std::size_t pos = 0; pos < body.size(); ++pos) {
        const auto byte = body[pos];
        std::size_t count = 1;
        if (byte == 0) {
            if (++pos == body.size()) {
                error = "truncated zero run";
                return std::nullopt;
            }
            count += body[pos];
        }
        if (count > header->decoded_size - message.payload.size()) {
            error = "zero-run expansion exceeds decoded size";
            return std::nullopt;
        }
        message.payload.insert(message.payload.end(), count, byte);
    }
    if (message.payload.size() != header->decoded_size) {
        error = "decoded body is shorter than declared size";
        return std::nullopt;
    }
    return message;
}

std::optional<std::vector<std::uint8_t>> Encode(PacketType type,
                                              std::span<const std::uint8_t> payload,
                                              std::string& error, bool compress) {
    error.clear();
    const auto expected = PayloadSize(type);
    if (!expected) {
        error = "unknown packet type";
        return std::nullopt;
    }
    if (payload.size() != *expected) {
        error = "payload size disagrees with packet type";
        return std::nullopt;
    }
    std::vector<std::uint8_t> encoded;
    if (compress && !payload.empty()) {
        encoded.reserve(payload.size());
        for (std::size_t pos = 0; pos < payload.size();) {
            const auto byte = payload[pos++];
            encoded.push_back(byte);
            if (byte == 0) {
                std::size_t count = 1;
                while (count < 256 && pos < payload.size() && payload[pos] == 0) {
                    ++count;
                    ++pos;
                }
                encoded.push_back(static_cast<std::uint8_t>(count - 1));
            }
        }
    }
    const bool compressed = compress && !payload.empty() && encoded.size() <= payload.size();
    const auto body = compressed ? std::span<const std::uint8_t>{encoded} : payload;
    std::vector<std::uint8_t> frame(HeaderSize + body.size(), 0);
    frame[0] = 0x00;
    frame[1] = 0x14;
    frame[2] = 0x45;
    frame[3] = 0x11;
    frame[4] = static_cast<std::uint8_t>(type);
    frame[5] = compressed ? 1 : 0;
    Write16(frame, 6, body.size());
    Write16(frame, 8, compressed ? payload.size() : 0);
    std::copy(body.begin(), body.end(), frame.begin() + HeaderSize);
    return frame;
}

bool StreamDecoder::Feed(std::span<const std::uint8_t> bytes, std::vector<Message>& output,
                         std::string& error) {
    error.clear();
    if (failed) {
        error = "stream failed; reset required";
        return false;
    }
    while (!bytes.empty()) {
        // Copy only as far as the current header/body boundary, even for a huge chunk.
        const auto target = header ? HeaderSize + header->encoded_size : HeaderSize;
        const auto count = std::min(bytes.size(), target - buffered);
        std::copy_n(bytes.begin(), count, buffer.begin() + buffered);
        buffered += count;
        bytes = bytes.subspan(count);
        if (!header && buffered == HeaderSize) {
            header = DecodeHeader(std::span{buffer}.first(HeaderSize), error);
            if (!header) {
                failed = true;
                return false;
            }
        }
        if (header && buffered == HeaderSize + header->encoded_size) {
            auto message = Decode(std::span{buffer}.first(buffered), error);
            if (!message) {
                failed = true;
                return false;
            }
            output.push_back(std::move(*message));
            buffered = 0;
            header.reset();
        }
    }
    return true;
}

void StreamDecoder::Reset() {
    buffered = 0;
    header.reset();
    failed = false;
}

std::size_t StreamDecoder::BufferedSize() const {
    return buffered;
}

bool StreamDecoder::Failed() const {
    return failed;
}
} // namespace Service::LDN::Mitm
