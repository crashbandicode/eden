// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone: link only ldn_mitm_codec.cpp, with src/ as an include directory.
#include "core/hle/service/ldn/ldn_mitm_codec.h"
#include "ldn_mitm_fixtures.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace Service::LDN::Mitm;
using Bytes = std::vector<std::uint8_t>;

void Require(bool condition, std::string_view what) {
    if (!condition) {
        throw std::runtime_error(std::string{what});
    }
}

Bytes Hex(std::string_view text) {
    Bytes result;
    const auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') {
            return static_cast<unsigned>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<unsigned>(c - 'a' + 10);
        }
        throw std::runtime_error("invalid fixture hex");
    };
    Require(text.size() % 2 == 0, "odd fixture hex");
    for (std::size_t i = 0; i < text.size(); i += 2) {
        result.push_back(static_cast<std::uint8_t>(nibble(text[i]) * 16 + nibble(text[i + 1])));
    }
    return result;
}

void GoldenFrames() {
    std::string error = "stale error";
    for (const auto& fixture : CodecFixtures::Frames) {
        const auto frame = Hex(fixture.hex);
        const auto type = static_cast<PacketType>(fixture.type);
        const Bytes payload(fixture.payload_size, 0);
        const auto message = Decode(frame, error);
        Require(message && error.empty(), fixture.name);
        Require(message->type == type && message->payload == payload, "golden decode");
        const auto encoded = Encode(type, payload, error);
        Require(encoded && *encoded == frame && error.empty(), "golden encode bytes");
        const auto header = DecodeHeader(frame, error);
        Require(header && header->type == type, "golden header");
    }
    // Different content and forced raw mode: raw decoded length and reserved bytes are ignored.
    const Bytes payload(64, 0x5a);
    auto raw = Encode(PacketType::Connect, payload, error, false);
    Require(raw && raw->size() == 76 && (*raw)[5] == 0 && (*raw)[8] == 0 &&
                (*raw)[9] == 0 && (*raw)[10] == 0 && (*raw)[11] == 0,
            "raw header emission");
    (*raw)[8] = 0xff;
    (*raw)[9] = 0xff;
    (*raw)[10] = 0xa5;
    (*raw)[11] = 0x5a;
    const auto decoded = Decode(*raw, error);
    Require(decoded && decoded->payload == payload, "ignored header fields");
    auto compressed = Hex(CodecFixtures::Frames[2].hex);
    compressed[10] = 0x82;
    compressed[11] = 0x19;
    Require(Decode(compressed, error).has_value(), "compressed reserved ignored");
}

void CompressionEdges() {
    struct Edge {
        std::size_t zeros;
        std::string_view body;
    };
    // One nonzero marker between two zero runs, concrete expected bytes.
    constexpr std::array<Edge, 5> edges{{
        {1, "00007e00ff00ff00ff00ff007d"},
        {2, "00017e00ff00ff00ff00ff007c"},
        {255, "00fe7e00ff00ff00ff007f"},
        {256, "00ff7e00ff00ff00ff007e"},
        {257, "00ff00007e00ff00ff00ff007d"},
    }};
    std::string error;
    for (const auto& edge : edges) {
        Bytes payload(PayloadSizeMax, 0);
        payload[edge.zeros] = 0x7e;
        const auto frame = Encode(PacketType::SyncNetwork, payload, error);
        const auto expected = Hex(edge.body);
        Require(frame && (*frame)[5] == 1, "edge compression selected");
        Require(Bytes(frame->begin() + HeaderSize, frame->end()) == expected,
                "concrete zero-run edge");
        const auto message = Decode(*frame, error);
        Require(message && message->payload == payload, "edge decode");
    }
    const Bytes no_zero(64, 0x71);
    auto frame = Encode(PacketType::Connect, no_zero, error);
    Require(frame && (*frame)[5] == 1 && frame->size() == 76,
            "equal size compression allowed");
    auto decoded = Decode(*frame, error);
    Require(decoded && decoded->payload == no_zero, "literal compressed decode");
    Bytes alternating(64);
    for (std::size_t i = 0; i < alternating.size(); ++i) {
        alternating[i] = static_cast<std::uint8_t>(i % 2 == 0 ? 0 : 0x71);
    }
    frame = Encode(PacketType::Connect, alternating, error);
    Require(frame && (*frame)[5] == 0 && frame->size() == 76, "expansion uses raw");
    decoded = Decode(*frame, error);
    Require(decoded && decoded->payload == alternating, "raw alternating decode");
    // Inbound compression may be larger than raw; the no-expansion choice is outbound only.
    auto expanded = Hex("001445110201600040000000");
    for (int i = 0; i < 32; ++i) {
        expanded.insert(expanded.end(), {0, 0, 0x71});
    }
    decoded = Decode(expanded, error);
    Require(decoded && decoded->payload == alternating, "expanded inbound compression");
    Bytes pair(64, 0x71);
    pair[0] = pair[1] = 0;
    frame = Encode(PacketType::Connect, pair, error);
    Require(frame && (*frame)[5] == 1 && (*frame)[12] == 0 && (*frame)[13] == 1,
            "two zeros fit without expansion");
    const Bytes all_zero(64, 0);
    frame = Encode(PacketType::Connect, all_zero, error, false);
    Require(frame && (*frame)[5] == 0 && frame->size() == 76, "explicit raw zeros");
    decoded = Decode(*frame, error);
    Require(decoded && decoded->payload == all_zero, "explicit raw decode");
}

void MalformedFrames() {
    const auto valid = Hex(CodecFixtures::Frames[2].hex);
    std::string error;
    const auto rejected = [&](const Bytes& frame) {
        Require(!Decode(frame, error) && !error.empty(), "malformed datagram rejected");
    };
    for (std::size_t length = 0; length < valid.size(); ++length) {
        rejected(Bytes(valid.begin(), valid.begin() + length));
    }
    auto bad = valid;
    bad.push_back(0x91);
    rejected(bad);
    for (const auto offset : {0u, 1u, 2u, 3u}) {
        bad = valid;
        bad[offset] ^= 1;
        rejected(bad);
    }
    for (const auto type : {4u, 5u, 255u}) {
        bad = valid;
        bad[4] = static_cast<std::uint8_t>(type);
        rejected(bad);
    }
    for (const auto flag : {2u, 255u}) {
        bad = valid;
        bad[5] = static_cast<std::uint8_t>(flag);
        rejected(bad);
    }
    for (const auto size : {63u, 65u, 1153u, 65535u}) {
        bad = valid;
        bad[8] = static_cast<std::uint8_t>(size & 255);
        bad[9] = static_cast<std::uint8_t>(size >> 8);
        rejected(bad);
    }
    for (const auto size : {2037u, 65535u}) {
        bad = valid;
        bad[6] = static_cast<std::uint8_t>(size & 255);
        bad[7] = static_cast<std::uint8_t>(size >> 8);
        Require(!DecodeHeader(bad, error), "oversize header rejected before body");
        rejected(bad);
    }
    bad = valid;
    bad[5] = 0;
    rejected(bad); // Raw Connect with two bytes.
    bad = valid;
    bad[4] = 0;
    rejected(bad); // Scan with Connect decoded length.
    bad = valid;
    bad[13] = 64;
    rejected(bad); // Expands to 65, one beyond Connect.
    bad[13] = 62;
    rejected(bad); // Expands to 63.
    bad = valid;
    bad[6] = 1;
    bad.pop_back();
    rejected(bad); // Dangling zero escape.
    bad = valid;
    bad[6] = 3;
    bad.push_back(0x71);
    rejected(bad); // Valid zero run followed by overflowing literal.
    bad = valid;
    bad[6] = 0;
    bad.resize(HeaderSize);
    rejected(bad); // Declared decoded bytes with no body.
    Require(!Encode(PacketType::Connect, Bytes(63), error), "short encode rejected");
    Require(!Encode(PacketType::ScanResp, Bytes(1153), error), "large encode rejected");
    Require(!Encode(static_cast<PacketType>(4), {}, error), "unknown encode rejected");
}

void TcpFragmentation() {
    std::string error;
    // Every boundary for every golden, both compressed and non-trigger raw content.
    std::vector<Bytes> frames;
    for (const auto& fixture : CodecFixtures::Frames) {
        frames.push_back(Hex(fixture.hex));
    }
    frames.push_back(*Encode(PacketType::ScanResp, Bytes(PayloadSizeMax, 0x69), error, false));
    for (const auto& frame : frames) {
        const auto expected = Decode(frame, error);
        for (std::size_t split = 0; split <= frame.size(); ++split) {
            StreamDecoder decoder;
            std::vector<Message> output;
            Require(decoder.Feed(std::span{frame}.first(split), output, error), "first split");
            Require(output.size() == (split == frame.size() ? 1u : 0u), "split publication");
            Require(decoder.Feed(std::span{frame}.subspan(split), output, error), "second split");
            Require(output.size() == 1 && output[0].type == expected->type &&
                        output[0].payload == expected->payload && decoder.BufferedSize() == 0,
                    "split complete bytes");
        }
        StreamDecoder drip;
        std::vector<Message> output;
        for (std::size_t i = 0; i < frame.size(); ++i) {
            Require(drip.Feed(std::span{frame}.subspan(i, 1), output, error), "byte drip");
            Require(drip.BufferedSize() <= FrameSizeMax, "bounded byte drip");
        }
        Require(output.size() == 1 && output[0].payload == expected->payload, "drip output");
    }
    Bytes batch;
    for (const auto& frame : frames) {
        batch.insert(batch.end(), frame.begin(), frame.end());
    }
    // Chunk larger than storage; storage must never depend on caller chunk length.
    Bytes huge;
    for (int i = 0; i < 100; ++i) {
        huge.insert(huge.end(), batch.begin(), batch.end());
    }
    StreamDecoder decoder;
    std::vector<Message> output;
    Require(decoder.Feed(huge, output, error) && output.size() == frames.size() * 100 &&
                decoder.BufferedSize() == 0,
            "coalesced chunk larger than buffer");
    output.clear();
    auto tail = frames.back();
    batch.insert(batch.end(), tail.begin(), tail.begin() + 19);
    Require(decoder.Feed(batch, output, error) && output.size() == frames.size() &&
                decoder.BufferedSize() == 19,
            "partial trailing frame retained");
    Require(decoder.Feed(std::span{tail}.subspan(19), output, error) &&
                output.size() == frames.size() + 1 && output.back().payload == Bytes(1152, 0x69),
            "next feed continues partial tail");
    // A corrupt frame cannot publish even its valid decompression prefix.
    for (const bool corrupt_body : {false, true}) {
        decoder.Reset();
        output.clear();
        auto bad = frames[2];
        bad[corrupt_body ? 13 : 0] ^= 0x80;
        Bytes chunk = frames[0];
        chunk.insert(chunk.end(), bad.begin(), bad.end());
        chunk.insert(chunk.end(), tail.begin(), tail.end());
        Require(!decoder.Feed(chunk, output, error) && decoder.Failed() && output.size() == 1,
                "terminal malformed stream with valid predecessor");
        Require(decoder.BufferedSize() <= FrameSizeMax, "failed storage bounded");
        Require(!decoder.Feed(frames[0], output, error) && output.size() == 1,
                "terminal stream refuses continuation");
        Require(!decoder.Feed({}, output, error), "empty feed preserves failure");
        decoder.Reset();
        Require(!decoder.Failed() && decoder.BufferedSize() == 0, "reset clears state");
        Require(decoder.Feed(tail, output, error) && output.size() == 2 && error.empty(),
                "restart after explicit reset");
    }
    decoder.Reset();
    output.clear();
    Require(decoder.Feed(std::span{tail}.first(17), output, error), "partial before reset");
    decoder.Reset();
    Require(decoder.Feed(frames[0], output, error) && output.size() == 1 &&
                decoder.BufferedSize() == 0,
            "reset discards incomplete prior connection");
}
} // namespace

int main() {
    try {
        GoldenFrames();
        CompressionEdges();
        MalformedFrames();
        TcpFragmentation();
        std::cout << "PASS GoldenFrames CompressionEdges MalformedFrames TcpFragmentation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL ldn_mitm_codec: " << error.what() << '\n';
        return 1;
    }
}
