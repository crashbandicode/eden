// SPDX-License-Identifier: GPL-3.0-or-later
// Original standalone transport probes, synthetic records only. Wire facts:
// spacemeowx2/ldn_mitm 9a2639eff5b2e60065be3dfae69c46de6c3d7648 and
// Ryujinx contributors c14ce4d2a5c9b373fb454906a6dc142c028d7be2.
// These are same-process loopback tests, not guest IPC or fresh-process evidence.
#include "core/hle/service/ldn/ldn_backend.h"
#include "core/hle/service/ldn/ldn_mitm_runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <boost/asio.hpp>

namespace {
using namespace Service::LDN::Mitm;
using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using Address = boost::asio::ip::address_v4;
using Error = boost::system::error_code;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr IPv4 HostIp{127, 0, 0, 1};
constexpr IPv4 ClientIp{127, 0, 0, 2};
constexpr auto Deadline = 3s;

void Require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error(std::string{reason});
    }
}

// Bind the error object by reference; inspect it after the operation completes,
// regardless of function argument evaluation order.
void RequireResult(bool condition, const std::string& error) {
    Require(condition, error.empty() ? std::string_view{"operation failed without error"} : error);
}

template <typename Predicate>
void Await(Predicate predicate, std::string_view reason, Clock::time_point end = Clock::now() + Deadline) {
    std::mutex mutex;
    std::condition_variable changed;
    std::unique_lock lock{mutex};
    while (!predicate()) {
        Require(Clock::now() < end, reason);
        // Bounded predicate polling; elapsed time is never an assertion of success.
        changed.wait_until(lock, std::min(end, Clock::now() + 1ms));
    }
}

bool Pending(Error error) {
    return error == boost::asio::error::would_block || error == boost::asio::error::try_again;
}

std::uint16_t AvailablePort() {
    boost::asio::io_context io;
    tcp::acceptor lease{io, tcp::endpoint{Address{HostIp}, 0}};
    const auto port = lease.local_endpoint().port();
    udp::socket datagram{io, udp::endpoint{Address{HostIp}, port}};
    // Reserve/check both protocols once. A conflict is a fixture failure, never retried.
    return port;
}

Configuration Config(std::uint16_t port, IPv4 ip = HostIp) {
    return {"synthetic-loopback", ip, {255, 0, 0, 0}, port};
}

Bytes Node(IPv4 ip, std::uint8_t slot = 0, bool connected = false) {
    Bytes node(64, 0);
    // Host-order numeric IPv4 serialized LE, independent of sockaddr byte order.
    node[0] = ip[3];
    node[1] = ip[2];
    node[2] = ip[1];
    node[3] = ip[0];
    node[0xa] = slot;
    node[0xb] = connected ? 1 : 0;
    node[0xc] = 't';
    node[0xd] = 'e';
    node[0xe] = 's';
    node[0xf] = 't';
    return node;
}

Bytes Network(IPv4 host = HostIp, std::uint8_t maximum = 8) {
    Bytes network(1152, 0);
    network[0x26] = 4; // SSID length
    network[0x27] = 'T';
    network[0x28] = 'E';
    network[0x29] = 'S';
    network[0x2a] = 'T';
    network[0x66] = maximum;
    network[0x67] = 1;
    const auto node = Node(host, 0, true);
    std::copy(node.begin(), node.end(), network.begin() + 0x68);
    network[0x26a] = 3;
    network[0x26c] = 'a';
    network[0x26d] = 'b';
    network[0x26e] = 'c';
    network[0x47f] = 0x91; // Opaque tail must survive transport unchanged.
    return network;
}

// Independent raw wire construction; no production Encode roundtrip oracle.
Bytes Raw(PacketType type, const Bytes& payload = {}) {
    Bytes frame{0, 0x14, 0x45, 0x11, static_cast<std::uint8_t>(type), 0,
                static_cast<std::uint8_t>(payload.size() & 255),
                static_cast<std::uint8_t>(payload.size() >> 8), 0, 0, 0, 0};
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

Bytes CompressedConnect() {
    // Concrete full frame for Node(127.0.0.2), independently checked saved bytes.
    return {0, 0x14, 0x45, 0x11, 2, 1, 12, 0, 64, 0, 0, 0,
            2, 0, 1, 127, 0, 7, 0x74, 0x65, 0x73, 0x74, 0, 47};
}

tcp::socket Dial(boost::asio::io_context& io, std::uint16_t port, IPv4 source = ClientIp) {
    tcp::socket socket{io};
    socket.open(tcp::v4());
    socket.bind(tcp::endpoint{Address{source}, 0});
    boost::asio::steady_timer timer{io};
    timer.expires_after(Deadline);
    Error result = boost::asio::error::timed_out;
    socket.async_connect(tcp::endpoint{Address{HostIp}, port}, [&](Error error) {
        result = error;
        timer.cancel();
    });
    timer.async_wait([&](Error error) {
        if (!error) {
            Error ignored;
            socket.cancel(ignored);
        }
    });
    io.restart();
    io.run();
    Require(!result, "loopback TCP connect failed or timed out");
    socket.non_blocking(true);
    return socket;
}

tcp::socket Accept(tcp::acceptor& listener, boost::asio::io_context& io) {
    listener.non_blocking(true);
    tcp::socket socket{io};
    Await([&] {
        Error error;
        listener.accept(socket, error);
        if (Pending(error)) {
            return false;
        }
        Require(!error, "fake host accept failed");
        return true;
    }, "fake host accept deadline");
    socket.non_blocking(true);
    return socket;
}

void Send(tcp::socket& socket, std::span<const std::uint8_t> bytes) {
    const auto end = Clock::now() + Deadline;
    Await([&] {
        if (bytes.empty()) {
            return true;
        }
        Error error;
        const auto count = socket.write_some(boost::asio::buffer(bytes.data(), bytes.size()), error);
        if (Pending(error)) {
            return false;
        }
        Require(!error && count != 0, "TCP write failed");
        bytes = bytes.subspan(count);
        return bytes.empty();
    }, "TCP write deadline", end);
}

void SendOrAlreadyClosed(tcp::socket& socket, std::span<const std::uint8_t> bytes) {
    Await([&] {
        Error error;
        const auto count = socket.write_some(boost::asio::buffer(bytes.data(), bytes.size()), error);
        if (Pending(error)) {
            return false;
        }
        if (error == boost::asio::error::broken_pipe || error == boost::asio::error::connection_reset ||
            error == boost::asio::error::eof) {
            return true; // Capacity refusal may close before the first write.
        }
        Require(!error && count != 0, "excess-peer write failed unexpectedly");
        bytes = bytes.subspan(count);
        return bytes.empty();
    }, "excess-peer write or refusal deadline");
}

Bytes ReadBytes(tcp::socket& socket, std::size_t size, Clock::time_point end) {
    Bytes bytes(size);
    std::size_t used = 0;
    Await([&] {
        if (used == size) {
            return true;
        }
        Error error;
        const auto count = socket.read_some(boost::asio::buffer(bytes.data() + used, size - used), error);
        if (Pending(error)) {
            return false;
        }
        Require(!error && count != 0, "unexpected TCP closure while reading frame");
        used += count;
        return used == size;
    }, "TCP read deadline", end);
    return bytes;
}

Message ReadFrame(tcp::socket& socket, Clock::time_point end = Clock::now() + Deadline) {
    auto frame = ReadBytes(socket, 12, end);
    const auto length = static_cast<std::size_t>(frame[6]) +
                        (static_cast<std::size_t>(frame[7]) << 8);
    Require(length <= 2036, "runtime emitted oversized frame");
    const auto body = ReadBytes(socket, length, end);
    frame.insert(frame.end(), body.begin(), body.end());
    std::string error;
    auto decoded = Decode(frame, error);
    Require(decoded.has_value(), "runtime emitted malformed frame");
    return std::move(*decoded);
}

template <typename Predicate>
Message ReadSync(tcp::socket& socket, Predicate predicate) {
    const auto end = Clock::now() + Deadline;
    for (;;) {
        auto message = ReadFrame(socket, end);
        Require(message.type == PacketType::SyncNetwork, "host emitted wrong TCP type");
        if (predicate(message.payload)) {
            return message;
        }
    }
}

void Closed(tcp::socket& socket) {
    std::array<std::uint8_t, 2048> discard{};
    Await([&] {
        Error error;
        socket.read_some(boost::asio::buffer(discard), error);
        if (Pending(error) || !error) {
            return false; // Drain any already queued valid synchronization frames.
        }
        Require(error == boost::asio::error::eof || error == boost::asio::error::connection_reset,
                "unexpected TCP closure error");
        return true;
    }, "TCP peer did not close by deadline");
}

void Close(tcp::socket& socket) {
    Error ignored;
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

udp::socket Udp(boost::asio::io_context& io, IPv4 source = ClientIp) {
    udp::socket socket{io, udp::endpoint{Address{source}, 0}};
    socket.set_option(boost::asio::socket_base::broadcast{true});
    socket.non_blocking(true);
    return socket;
}

void SendUdp(udp::socket& socket, std::uint16_t port, const Bytes& bytes, IPv4 target = HostIp) {
    Await([&] {
        Error error;
        const auto count = socket.send_to(boost::asio::buffer(bytes), udp::endpoint{Address{target}, port}, 0, error);
        if (Pending(error)) {
            return false;
        }
        Require(!error && count == bytes.size(), "UDP send failed");
        return true;
    }, "UDP send deadline");
}

std::pair<Bytes, udp::endpoint> ReadUdp(udp::socket& socket) {
    std::array<std::uint8_t, 2049> bytes{};
    udp::endpoint sender;
    std::size_t size = 0;
    Await([&] {
        Error error;
        size = socket.receive_from(boost::asio::buffer(bytes), sender, 0, error);
        if (Pending(error)) {
            return false;
        }
        Require(!error, "UDP receive failed");
        return true;
    }, "UDP reply deadline");
    return {Bytes(bytes.begin(), bytes.begin() + size), sender};
}

struct Events {
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<Event> events;

    void Add(const Event& event) {
        std::scoped_lock lock{mutex};
        events.push_back(event);
        changed.notify_all();
    }
    std::vector<Event> Snapshot() const {
        std::scoped_lock lock{mutex};
        return events;
    }
    std::size_t Count(Event::Kind kind, PacketType type = PacketType::ScanResp) const {
        const auto copy = Snapshot();
        return static_cast<std::size_t>(std::count_if(copy.begin(), copy.end(), [&](const Event& event) {
            return event.kind == kind && (kind != Event::Kind::Message || event.message.type == type);
        }));
    }
    Event Wait(Event::Kind kind, std::uint8_t slot, std::size_t occurrence = 1) {
        std::unique_lock lock{mutex};
        const auto predicate = [&] {
            return std::count_if(events.begin(), events.end(), [&](const Event& event) {
                return event.kind == kind && event.slot == slot;
            }) >= static_cast<std::ptrdiff_t>(occurrence);
        };
        Require(changed.wait_for(lock, Deadline, predicate), "expected runtime event deadline");
        std::size_t found = 0;
        for (const auto& event : events) {
            if (event.kind == kind && event.slot == slot && ++found == occurrence) {
                return event;
            }
        }
        throw std::runtime_error("event predicate disagrees with saved events");
    }
};

// Only the minimal guest responsibility is modeled: accept assigned slots and
// republish membership. Socket ownership, allocation and validation stay production code.
struct Host {
    Events events;
    std::mutex network_mutex;
    Bytes network{Network()};
    Runtime runtime; // Destroy before callback-owned data, including on constructor failure.

    explicit Host(std::uint16_t port) {
        std::string error;
        Require(runtime.Attach(this, [this](const Event& event) {
            if (event.slot > 0 &&
                (event.kind == Event::Kind::PeerClosed || event.kind == Event::Kind::Message)) {
                Bytes update;
                {
                    std::scoped_lock lock{network_mutex};
                    const auto offset = 0x68 + static_cast<std::size_t>(event.slot) * 64;
                    const bool was_connected = network[offset + 0xb] != 0;
                    if (event.kind == Event::Kind::Message && event.message.type == PacketType::Connect) {
                        std::copy(event.message.payload.begin(), event.message.payload.end(), network.begin() + offset);
                        network[offset + 0xa] = event.slot;
                        network[offset + 0xb] = 1;
                        if (!was_connected) {
                            ++network[0x67];
                        }
                    } else if (event.kind == Event::Kind::PeerClosed && was_connected) {
                        std::fill_n(network.begin() + offset, 64, 0);
                        --network[0x67];
                    }
                    update = network;
                }
                runtime.PublishNetwork(std::move(update));
            }
            events.Add(event);
        }, error), "host attach failed");
        RequireResult(runtime.Start(Config(port), error), error);
        runtime.PublishNetwork(network);
        Await([&] { return runtime.GetStatus().role == "host"; }, "host publication deadline");
    }
    ~Host() {
        runtime.Detach(this);
    }
    void Mark(std::uint8_t marker) {
        Bytes update;
        {
            std::scoped_lock lock{network_mutex};
            network[0x47f] = marker;
            update = network;
        }
        runtime.PublishNetwork(std::move(update));
    }
};

void BackendSelection() {
    using Service::LDN::SelectSocketBackend;
    using B = Service::LDN::SocketBackend;
    struct Row { bool mitm, automatic, room; B datagram, stream; };
    constexpr std::array<Row, 8> table{{
        {false, false, false, B::Host, B::Host},
        {false, false, true, B::Room, B::Room},
        {false, true, false, B::AutomaticLanOnline, B::Host},
        {false, true, true, B::AutomaticLanOnline, B::Host},
        {true, false, false, B::Host, B::Host},
        {true, false, true, B::Host, B::Host},
        {true, true, false, B::Host, B::Host},
        {true, true, true, B::Host, B::Host},
    }};
    for (const auto& row : table) {
        Require(SelectSocketBackend(row.mitm, row.automatic, row.room, true) == row.datagram,
                "datagram backend selection");
        Require(SelectSocketBackend(row.mitm, row.automatic, row.room, false) == row.stream,
                "stream backend selection");
    }
}

void RecordBounds() {
    std::string error;
    const auto good = Network();
    Require(ValidateNetwork(good, error) && ValidateNode(Node(ClientIp), error), "synthetic valid records");
    const auto compressed = Decode(CompressedConnect(), error);
    Require(compressed && compressed->payload == Node(ClientIp), "literal compressed Connect bytes");
    for (const auto size : {63u, 65u}) {
        Require(!ValidateNode(Bytes(size), error), "node exact size");
    }
    for (const auto field : {0xau, 0xbu}) {
        auto node = Node(ClientIp);
        node[field] = field == 0xa ? 8 : 2;
        Require(!ValidateNode(node, error), "node bounds");
    }
    for (const auto size : {1151u, 1153u}) {
        Require(!ValidateNetwork(Bytes(size), error), "network exact size");
    }
    for (const auto& [offset, value] : std::array<std::pair<std::size_t, std::uint8_t>, 8>{{
             {0x26, 33}, {0x66, 0}, {0x66, 9}, {0x67, 0}, {0x67, 9},
             {0x26a, 0x81}, {0x68 + 0xa, 1}, {0x68 + 0xb, 0}}}) {
        auto bad = good;
        bad[offset] = value;
        if (offset == 0x26a) {
            bad[0x26b] = 1; // 385 bytes, one above advertised data capacity.
        }
        Require(!ValidateNetwork(bad, error) && !error.empty(), "network field bounds");
    }
    auto sparse = Network(HostIp, 4);
    sparse[0x67] = 2;
    const auto seventh = Node(ClientIp, 7, true);
    std::copy(seventh.begin(), seventh.end(), sparse.begin() + 0x68 + 7 * 64);
    Require(ValidateNetwork(sparse, error), "sparse slot7 max4 count2 is legal");
    sparse[0x68 + 7 * 64 + 0xa] = 6;
    Require(!ValidateNetwork(sparse, error), "connected node ID must match fixed slot");
    auto limit = Network();
    limit[0x26] = 32;
    limit[0x26a] = 0x80;
    limit[0x26b] = 1;
    limit[0x67] = 8;
    for (std::uint8_t slot = 1; slot < 8; ++slot) {
        const auto node = Node({127, 0, 0, static_cast<std::uint8_t>(slot + 1)}, slot, true);
        std::copy(node.begin(), node.end(), limit.begin() + 0x68 + slot * 64);
    }
    Require(ValidateNetwork(limit, error), "inclusive record maxima");
    limit[0x66] = 7;
    Require(!ValidateNetwork(limit, error), "connected count above capacity");
}

void UdpDiscovery() {
    const auto port = AvailablePort();
    Host host{port};
    boost::asio::io_context io;
    auto sender = Udp(io);
    Require(sender.local_endpoint().port() != port, "UDP source must be ephemeral");
    const auto check_reply = [&] {
        const auto [bytes, endpoint] = ReadUdp(sender);
        Require(endpoint.address() == Address{HostIp} && endpoint.port() == port, "UDP reply source");
        std::string error;
        const auto reply = Decode(bytes, error);
        Require(reply && reply->type == PacketType::ScanResp && reply->payload == Network(),
                "UDP ephemeral-source reply and opaque bytes");
    };
    SendUdp(sender, port, Raw(PacketType::Scan));
    check_reply();
    SendUdp(sender, port, Raw(PacketType::Scan), {127, 255, 255, 255});
    check_reply(); // Actual loopback broadcast, not a simulated broadcast calculation.
    udp::socket monitor{io};
    monitor.open(udp::v4());
    monitor.set_option(udp::socket::reuse_address{true});
    monitor.bind(udp::endpoint{Address{IPv4{127, 255, 255, 255}}, port});
    monitor.non_blocking(true);
    std::string scan_error;
    Require(host.runtime.Scan(scan_error), "runtime Scan admission");
    const auto [scan_bytes, scan_source] = ReadUdp(monitor);
    Require(scan_bytes == Raw(PacketType::Scan) && scan_source.address() == Address{HostIp} &&
                scan_source.port() == port, "runtime Scan broadcasts the concrete raw frame");
    const auto baseline = host.runtime.GetStatus().malformed;
    auto invalid_magic = Raw(PacketType::Scan);
    invalid_magic[0] = 1;
    SendUdp(sender, port, invalid_magic);
    SendUdp(sender, port, Raw(PacketType::Connect, Node(ClientIp)));
    auto invalid_network = Network();
    invalid_network[0x67] = 8;
    SendUdp(sender, port, Raw(PacketType::ScanResp, invalid_network));
    SendUdp(sender, port, Bytes(2049, 0x71));
    Await([&] { return host.runtime.GetStatus().malformed == baseline + 4; }, "malformed UDP counter");
    Require(host.events.Count(Event::Kind::Message) == 0, "malformed UDP must not deliver");
    SendUdp(sender, port, Raw(PacketType::ScanResp, Network()));
    const auto event = host.events.Wait(Event::Kind::Message, 0);
    Require(event.message.type == PacketType::ScanResp && event.message.payload == Network() &&
                event.peer == ClientIp, "valid raw UDP response delivery");
    Require(host.events.Count(Event::Kind::Message) == 1, "UDP publication fence excludes malformed packets");
}

void TcpStationFrames() {
    const auto port = AvailablePort();
    boost::asio::io_context io;
    tcp::acceptor fake_host{io, tcp::endpoint{Address{HostIp}, port}};
    Events events;
    Runtime station;
    std::string error;
    Require(station.Attach(&events, [&](const Event& event) { events.Add(event); }, error), "station attach");
    RequireResult(station.Start(Config(port, ClientIp), error), error);
    const auto connect = [&] {
        RequireResult(station.Connect(HostIp, Node(ClientIp), error), error);
        auto socket = Accept(fake_host, io);
        const auto hello = ReadFrame(socket);
        Require(hello.type == PacketType::Connect && hello.payload == Node(ClientIp), "station Connect source mapping");
        return socket;
    };
    auto peer = connect();
    auto first_network = Network();
    auto second_network = Network();
    second_network[0x47f] = 0x92;
    const auto first = Raw(PacketType::SyncNetwork, first_network);
    Send(peer, std::span{first}.first(7));
    auto datagram = Udp(io, {127, 0, 0, 3});
    SendUdp(datagram, port, Raw(PacketType::ScanResp, Network()), ClientIp);
    events.Wait(Event::Kind::Message, 0);
    Require(events.Count(Event::Kind::Message, PacketType::SyncNetwork) == 0,
            "partial TCP header has no published sync");
    Bytes rest(first.begin() + 7, first.end());
    const auto second = Raw(PacketType::SyncNetwork, second_network);
    rest.insert(rest.end(), second.begin(), second.end());
    Send(peer, rest);
    Await([&] { return events.Count(Event::Kind::Message, PacketType::SyncNetwork) == 2; }, "coalesced sync delivery");
    std::vector<Bytes> syncs;
    for (const auto& event : events.Snapshot()) {
        if (event.kind == Event::Kind::Message && event.message.type == PacketType::SyncNetwork) {
            Require(event.slot == 0 && event.peer == HostIp, "upstream host uses slot0");
            syncs.push_back(event.message.payload);
        }
    }
    Require(syncs == std::vector<Bytes>{first_network, second_network}, "ordered split/coalesced payloads");
    Require(station.GetStatus().role == "station" && station.GetStatus().peers == 1,
            "valid upstream sync establishes station state");
    const auto bad_before = station.GetStatus().malformed;
    Send(peer, Raw(PacketType::Scan)); // Wrong role on upstream TCP.
    events.Wait(Event::Kind::PeerClosed, 0);
    Closed(peer);
    Require(station.GetStatus().malformed == bad_before + 1, "wrong-role TCP rejected");
    Require(station.GetStatus().role == "idle" && station.GetStatus().peers == 0,
            "upstream closure clears station state");
    peer = connect();
    Send(peer, std::span{first}.first(20));
    Close(peer);
    events.Wait(Event::Kind::PeerClosed, 0, 2);
    Require(station.GetStatus().malformed == bad_before + 2, "partial-body EOF malformed");
    Require(events.Count(Event::Kind::Message, PacketType::SyncNetwork) == 2, "partial EOF must not deliver");
    peer = connect();
    auto wrong_host = Network({127, 0, 0, 4});
    Send(peer, Raw(PacketType::SyncNetwork, wrong_host));
    events.Wait(Event::Kind::PeerClosed, 0, 3);
    Closed(peer);
    Require(station.GetStatus().malformed == bad_before + 3, "network host IP must match TCP source");
    peer = connect();
    auto valid_then_bad = first;
    auto bad_header = Raw(PacketType::Scan);
    bad_header[0] = 1;
    valid_then_bad.insert(valid_then_bad.end(), bad_header.begin(), bad_header.end());
    Send(peer, valid_then_bad);
    events.Wait(Event::Kind::PeerClosed, 0, 4);
    Closed(peer);
    Require(station.GetStatus().malformed == bad_before + 4 &&
                events.Count(Event::Kind::Message, PacketType::SyncNetwork) == 3,
            "valid Sync preceding malformed TCP header is delivered exactly once");
    station.Detach(&events);
}

void StableSlots() {
    const auto port = AvailablePort();
    Host host{port};
    boost::asio::io_context io;
    auto pending = Dial(io, port);
    const auto compressed = CompressedConnect();
    Send(pending, std::span{compressed}.first(5));
    auto second = Dial(io, port, {127, 0, 0, 3});
    Send(second, Raw(PacketType::Connect, Node({127, 0, 0, 3})));
    Require(host.events.Wait(Event::Kind::Message, 2).peer == IPv4{127, 0, 0, 3}, "pending handshake reserves slot1");
    Send(pending, std::span{compressed}.subspan(5));
    Require(host.events.Wait(Event::Kind::Message, 1).peer == ClientIp, "earlier pending handshake retains slot1");
    auto third = Dial(io, port, {127, 0, 0, 4});
    Send(third, Raw(PacketType::Connect, Node({127, 0, 0, 4})));
    host.events.Wait(Event::Kind::Message, 3);
    Require(host.runtime.GetStatus().peers == 3, "joined peer count");
    Close(second);
    const auto removed = host.events.Wait(Event::Kind::PeerClosed, 2);
    auto replacement = Dial(io, port, {127, 0, 0, 5});
    Send(replacement, Raw(PacketType::Connect, Node({127, 0, 0, 5})));
    const auto replacement_event = host.events.Wait(Event::Kind::Message, 2, 2);
    Require(replacement_event.peer == IPv4{127, 0, 0, 5} && removed.connection != 0 &&
                replacement_event.connection != removed.connection, "middle slot reuse has a new connection token");
    host.runtime.ClosePeer(2, removed.connection);
    host.Mark(0xa5);
    const auto sync = ReadSync(third, [](const Bytes& bytes) { return bytes[0x47f] == 0xa5; });
    Require(sync.payload[0x67] == 4 && sync.payload[0x68 + 3 * 64 + 0xa] == 3 &&
                sync.payload[0x68 + 3 * 64] == 4 && sync.payload[0x68 + 2 * 64] == 5,
            "survivor slots do not shift after reuse");
    const auto replacement_sync = ReadSync(replacement, [](const Bytes& bytes) { return bytes[0x47f] == 0xa5; });
    Require(replacement_sync.payload == sync.payload && host.runtime.GetStatus().peers == 3,
            "stale targeted close leaves replacement connection alive");
    const auto malformed = host.runtime.GetStatus().malformed;
    // Valid second Connect coalesced with a forbidden Scan: publish the valid frame, then close.
    auto mixed = Dial(io, port, {127, 0, 0, 6});
    auto chunk = Raw(PacketType::Connect, Node({127, 0, 0, 6}));
    const auto wrong_role = Raw(PacketType::Scan);
    chunk.insert(chunk.end(), wrong_role.begin(), wrong_role.end());
    Send(mixed, chunk);
    host.events.Wait(Event::Kind::Message, 4);
    host.events.Wait(Event::Kind::PeerClosed, 4);
    Closed(mixed);
    Require(host.runtime.GetStatus().malformed == malformed + 1 && host.runtime.GetStatus().peers == 3,
            "coalesced valid predecessor then wrong-role close");
    auto wrong_ip = Dial(io, port, {127, 0, 0, 7});
    Send(wrong_ip, Raw(PacketType::Connect, Node(ClientIp)));
    Closed(wrong_ip);
    Require(host.runtime.GetStatus().malformed == malformed + 2, "Connect address must match source");
    const auto before_timeout = host.events.Count(Event::Kind::Message, PacketType::Connect);
    auto unfinished = Dial(io, port, {127, 0, 0, 8});
    Send(unfinished, std::span{compressed}.first(1));
    Closed(unfinished);
    Require(host.runtime.GetStatus().last_error.find("timed out") != std::string::npos &&
                host.events.Count(Event::Kind::Message, PacketType::Connect) == before_timeout &&
                host.runtime.GetStatus().peers == 3,
            "expired pending handshake releases reservation without a joined event");
    auto after_timeout = Dial(io, port, {127, 0, 0, 9});
    Send(after_timeout, Raw(PacketType::Connect, Node({127, 0, 0, 9})));
    host.events.Wait(Event::Kind::Message, 4, 2);
}

void FullCapacity() {
    const auto port = AvailablePort();
    Host host{port};
    boost::asio::io_context io;
    std::vector<tcp::socket> peers;
    for (std::uint8_t slot = 1; slot < 8; ++slot) {
        const IPv4 ip{127, 0, 0, static_cast<std::uint8_t>(slot + 1)};
        peers.push_back(Dial(io, port, ip));
        Send(peers.back(), Raw(PacketType::Connect, Node(ip)));
        host.events.Wait(Event::Kind::Message, slot);
    }
    Require(host.runtime.GetStatus().peers == 7, "seven-peer capacity");
    auto excess = Dial(io, port, {127, 0, 0, 9});
    Closed(excess);
    Require(host.events.Count(Event::Kind::Message, PacketType::Connect) == 7 &&
                host.runtime.GetStatus().peers == 7, "eighth peer not admitted");
    host.Mark(0xa6);
    const auto full = ReadSync(peers.back(), [](const Bytes& bytes) { return bytes[0x47f] == 0xa6; });
    Require(full.payload[0x67] == 8, "full host membership count");
    Close(peers[3]);
    host.events.Wait(Event::Kind::PeerClosed, 4);
    auto reuse = Dial(io, port, {127, 0, 0, 10});
    Send(reuse, Raw(PacketType::Connect, Node({127, 0, 0, 10})));
    Require(host.events.Wait(Event::Kind::Message, 4, 2).peer == IPv4{127, 0, 0, 10}, "full-capacity vacancy reuse");
    Require(host.runtime.GetStatus().peers == 7, "capacity restored");
    // Keep the high stable slot while lowering max connected count. Capacity is
    // cardinality, not the highest available node ID; pending reservations count too.
    for (std::uint8_t slot = 1; slot < 7; ++slot) {
        if (slot == 4) {
            Close(reuse);
            host.events.Wait(Event::Kind::PeerClosed, slot, 2);
        } else {
            Close(peers[slot - 1]);
            host.events.Wait(Event::Kind::PeerClosed, slot);
        }
    }
    Require(host.runtime.GetStatus().peers == 1, "high slot survivor after removals");
    {
        Bytes update;
        {
            std::scoped_lock lock{host.network_mutex};
            host.network[0x66] = 4;
            host.network[0x47f] = 0xa7;
            update = host.network;
        }
        host.runtime.PublishNetwork(std::move(update));
    }
    const auto sparse = ReadSync(peers.back(), [](const Bytes& bytes) { return bytes[0x47f] == 0xa7; });
    Require(sparse.payload[0x66] == 4 && sparse.payload[0x67] == 2 &&
                sparse.payload[0x68 + 7 * 64 + 0xa] == 7,
            "sparse slot7 survives lower count capacity");
    auto reserved = Dial(io, port, {127, 0, 0, 11});
    const Bytes magic_prefix{0};
    Send(reserved, magic_prefix);
    auto joined = Dial(io, port, {127, 0, 0, 12});
    Send(joined, Raw(PacketType::Connect, Node({127, 0, 0, 12})));
    host.events.Wait(Event::Kind::Message, 2, 2);
    auto over_sparse_capacity = Dial(io, port, {127, 0, 0, 13});
    const auto before_excess = host.events.Count(Event::Kind::Message, PacketType::Connect);
    SendOrAlreadyClosed(over_sparse_capacity, Raw(PacketType::Connect, Node({127, 0, 0, 13})));
    Await([&] {
        Require(host.events.Count(Event::Kind::Message, PacketType::Connect) == before_excess,
                "sparse count capacity admitted an excess handshake");
        std::array<std::uint8_t, 2048> bytes{};
        Error closed;
        over_sparse_capacity.read_some(boost::asio::buffer(bytes), closed);
        if (Pending(closed) || !closed) {
            return false;
        }
        Require(closed == boost::asio::error::eof || closed == boost::asio::error::connection_reset,
                "unexpected excess-peer closure error");
        return true;
    }, "sparse excess peer must close before its handshake is published");
    Require(host.runtime.GetStatus().peers == 2, "pending plus sparse survivor reserves full count capacity");
    const auto reserved_hello = Raw(PacketType::Connect, Node({127, 0, 0, 11}));
    // The one-byte prefix above is the same magic byte in the independent raw frame.
    Send(reserved, std::span{reserved_hello}.subspan(1));
    host.events.Wait(Event::Kind::Message, 1, 2);
    Require(host.runtime.GetStatus().peers == 3, "sparse full count after pending handshake");
}

void RuntimeLifecycle() {
    const auto port = AvailablePort();
    boost::asio::io_context io;
    Events events;
    int other_owner = 0;
    Runtime runtime;
    std::string error;
    Require(runtime.Attach(&events, [&](const Event& event) { events.Add(event); }, error), "first owner attach");
    Require(runtime.Attach(&events, [&](const Event& event) { events.Add(event); }, error), "same owner may reattach");
    Require(!runtime.Attach(&other_owner, {}, error) && !error.empty(), "foreign owner rejected");
    runtime.Detach(&other_owner);
    RequireResult(runtime.Start(Config(port), error), error);
    Require(runtime.Start(Config(port), error) && runtime.GetStatus().running, "idempotent same-config start");
    Require(events.Count(Event::Kind::Stopped) == 0, "idempotent start must not stop");
    runtime.Stop();
    runtime.Stop();
    Require(events.Count(Event::Kind::Stopped) == 1 && !runtime.GetStatus().running, "idempotent stop");
    Require(!runtime.Scan(error) && !error.empty(), "scan on stopped runtime");
    Require(!runtime.Connect(HostIp, Node(ClientIp), error) && !error.empty(), "connect on stopped runtime");
    Require(runtime.Start(Config(port), error), "restart after stop");
    const auto changed_port = AvailablePort();
    Require(runtime.Start(Config(changed_port), error), "changed-config restart");
    Require(runtime.GetStatus().configuration.port == changed_port && events.Count(Event::Kind::Stopped) == 2,
            "changed config stopped old runtime");
    runtime.Detach(&events);
    Require(runtime.Attach(&other_owner, {}, error), "detach releases owner");
    runtime.Detach(&other_owner);
    // TCP-only blocker forces failure after UDP sockets were opened, challenging partial cleanup.
    tcp::acceptor blocker{io, tcp::endpoint{Address{HostIp}, port}};
    Require(!runtime.Start(Config(port), error) && !error.empty() &&
                !runtime.GetStatus().running && runtime.GetStatus().last_error == error,
            "real bind failure propagated");
    blocker.close();
    Require(runtime.Start(Config(port), error), "bind failure releases partial UDP state");
    runtime.Stop();
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{}, released{}, timed_out{};

    void Enter() {
        std::unique_lock lock{mutex};
        entered = true;
        changed.notify_all();
        if (!changed.wait_for(lock, Deadline, [&] { return released; })) {
            timed_out = true;
        }
    }
    void Wait() {
        std::unique_lock lock{mutex};
        Require(changed.wait_for(lock, Deadline, [&] { return entered; }), "callback did not reach barrier");
    }
    void Release() {
        std::scoped_lock lock{mutex};
        released = true;
        changed.notify_all();
    }
};

// All failure paths release the callback before joining. A real product join
// deadlock fails this standalone process at its deadline instead of detaching a
// thread that could retain destroyed test data or leave a hidden live job.
struct JoinTask {
    Gate& gate;
    std::promise<void> completed;
    std::future<void> done{completed.get_future()};
    std::thread thread;

    template <typename Operation>
    JoinTask(Gate& gate_, Operation operation) : gate{gate_}, thread{[this, operation] {
        try {
            operation();
            completed.set_value();
        } catch (...) {
            completed.set_exception(std::current_exception());
        }
    }} {}
    bool Ready() { return done.wait_for(0s) == std::future_status::ready; }
    void Join() {
        gate.Release();
        if (thread.joinable()) {
            if (done.wait_for(Deadline) != std::future_status::ready) {
                std::cerr << "FAIL teardown did not join within deadline\n" << std::flush;
                std::_Exit(1);
            }
            thread.join();
        }
    }
    ~JoinTask() { Join(); }
};

struct ReleaseGate {
    Gate& gate;
    ~ReleaseGate() { gate.Release(); }
};

void TeardownAndCommandBounds() {
    // With ordinary commands saturated, lifecycle intent remains accepted. Both
    // replacement directions must move to the tail, rather than revive stale state.
    for (const bool final_host : {false, true}) {
        const auto port = AvailablePort();
        boost::asio::io_context io;
        Gate gate;
        Events events;
        Runtime runtime;
        ReleaseGate release{gate};
        std::string error;
        Require(runtime.Attach(&events, [&](const Event& event) {
            events.Add(event);
            if (event.kind == Event::Kind::Message) {
                gate.Enter();
            }
        }, error), "ordered lifecycle receiver attach");
        RequireResult(runtime.Start(Config(port), error), error);
        auto sender = Udp(io);
        SendUdp(sender, port, Raw(PacketType::ScanResp, Network()));
        gate.Wait();
        for (int i = 0; i < 64; ++i) {
            Require(runtime.Scan(error), "ordinary capacity available before limit");
        }
        Require(!runtime.Scan(error) && !error.empty() &&
                    !runtime.GetStatus().last_error.empty() && runtime.GetStatus().pending_commands == 64,
                "ordinary cap rejects explicitly while callback held");
        auto previous = Network();
        previous[0x47f] = 0xb1;
        auto latest = Network();
        latest[0x47f] = 0xb2;
        runtime.PublishNetwork(previous);
        runtime.Leave();
        runtime.PublishNetwork(latest);
        if (!final_host) {
            runtime.PublishNetwork(std::nullopt);
        }
        Require(runtime.GetStatus().pending_commands == 66,
                "snapshot and leave retain reserved bounded capacity under saturation");
        gate.Release();
        Await([&] {
            const auto status = runtime.GetStatus();
            return status.pending_commands == 0 && status.role == (final_host ? "host" : "idle");
        }, "latest lifecycle intent must become actual role");
        if (final_host) {
            SendUdp(sender, port, Raw(PacketType::Scan));
            const auto [bytes, source] = ReadUdp(sender);
            const auto reply = Decode(bytes, error);
            Require(source.port() == port && reply && reply->type == PacketType::ScanResp &&
                        reply->payload == latest, "latest queued snapshot reaches actual UDP listener");
            auto peer = Dial(io, port);
            Send(peer, CompressedConnect());
            events.Wait(Event::Kind::Message, 1);
            Require(runtime.GetStatus().peers == 1, "latest host intent accepts a real TCP peer");
        } else {
            auto peer = Dial(io, port);
            Closed(peer);
            Require(runtime.GetStatus().peers == 0, "latest leave intent rejects TCP membership");
        }
        runtime.Detach(&events);
        Require(!gate.timed_out, "ordered lifecycle callback released before deadline");
    }
    for (const bool detach : {false, true}) {
        const auto port = AvailablePort();
        boost::asio::io_context io;
        Gate gate;
        Events events;
        bool callback_posts_finished = false;
        bool callback_rejected = false;
        Runtime runtime;
        ReleaseGate release{gate};
        std::string error;
        Require(runtime.Attach(&events, [&](const Event& event) {
            events.Add(event);
            if (event.kind == Event::Kind::Message && !callback_posts_finished) {
                gate.Enter();
                runtime.PublishNetwork(Network());
                std::string scan_error, connect_error;
                const bool scan = runtime.Scan(scan_error);
                const bool connect = runtime.Connect(HostIp, Node(HostIp), connect_error);
                runtime.Leave();
                runtime.ClosePeer(1);
                static_cast<void>(runtime.GetStatus());
                callback_rejected = !scan && !connect && !scan_error.empty() && !connect_error.empty();
                callback_posts_finished = true;
            }
        }, error), "barrier receiver attach");
        RequireResult(runtime.Start(Config(port), error), error);
        auto sender = Udp(io);
        SendUdp(sender, port, Raw(PacketType::ScanResp, Network()));
        gate.Wait();
        std::size_t accepted = 0, rejected = 0;
        for (int i = 0; i < 96; ++i) {
            if (runtime.Scan(error)) {
                ++accepted;
            } else {
                ++rejected;
                Require(!error.empty(), "full command queue must return an error");
            }
            Require(runtime.GetStatus().pending_commands <= 64, "pending command hard bound");
        }
        Require(accepted == 64 && rejected == 32 && runtime.GetStatus().pending_commands == 64 &&
                    !runtime.GetStatus().last_error.empty(), "held callback exposes exact command cap");
        runtime.PublishNetwork(Network());
        runtime.Leave();
        for (std::uint8_t slot = 0; slot < 8; ++slot) {
            runtime.ClosePeer(slot);
        }
        Require(runtime.GetStatus().pending_commands == 74 && !runtime.Scan(error) && !error.empty(),
                "reserved lifecycle classes are bounded alongside full ordinary queue");
        JoinTask stopping{gate, [&] {
            if (detach) {
                runtime.Detach(&events);
            } else {
                runtime.Stop();
            }
        }};
        Await([&] {
            return !runtime.Scan(error) && error.find("stopped") != std::string::npos;
        }, "Stop must bypass full queue and disable publication");
        Require(!stopping.Ready(), "teardown must wait for active callback");
        stopping.Join();
        stopping.done.get();
        Require(callback_posts_finished && callback_rejected && !gate.timed_out,
                "callback can post while Stop/Detach waits without deadlock");
        Require(!runtime.GetStatus().running && runtime.GetStatus().pending_commands == 0 &&
                    events.Count(Event::Kind::Stopped) == 1, "teardown drains bounded work and signals once");
        const auto delivered = events.Count(Event::Kind::Message);
        Require(runtime.Start(Config(port), error), "restart after callback teardown");
        SendUdp(sender, port, Raw(PacketType::ScanResp, Network()));
        Await([&] { return runtime.GetStatus().received == 1; }, "post-restart UDP processing fence");
        if (detach) {
            Require(events.Count(Event::Kind::Message) == delivered, "no delivery after Detach returns");
            int replacement_owner = 0;
            Require(runtime.Attach(&replacement_owner, [&](const Event& event) { events.Add(event); }, error),
                    "new owner after detached restart");
            SendUdp(sender, port, Raw(PacketType::ScanResp, Network()));
            Await([&] { return events.Count(Event::Kind::Message) == delivered + 1; }, "new owner receives");
            runtime.Detach(&replacement_owner);
        } else {
            Await([&] { return events.Count(Event::Kind::Message) == delivered + 1; }, "Stop preserves owner for restart");
            runtime.Detach(&events);
        }
    }
}
} // namespace

int main() {
    constexpr std::array<std::pair<std::string_view, void (*)()>, 8> groups{{
        {"BackendSelection", BackendSelection}, {"RecordBounds", RecordBounds},
        {"UdpDiscovery", UdpDiscovery}, {"TcpStationFrames", TcpStationFrames},
        {"StableSlots", StableSlots}, {"FullCapacity", FullCapacity},
        {"RuntimeLifecycle", RuntimeLifecycle}, {"TeardownAndCommandBounds", TeardownAndCommandBounds},
    }};
    std::size_t passed = 0;
    for (const auto& [name, run] : groups) {
        try {
            run();
            ++passed;
            std::cout << "PASS " << name << '\n' << std::flush;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    if (passed != groups.size()) {
        std::cerr << "FAIL " << (groups.size() - passed) << " of " << groups.size() << " groups\n";
        return 1;
    }
    std::cout << "PASS 8 groups; same-process IPv4 loopback only\n";
    return 0;
}
