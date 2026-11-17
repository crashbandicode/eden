// SPDX-License-Identifier: GPL-3.0-or-later
// Original transport for the published ldn_mitm protocol; see docs/developer/ldn-mitm.md.
#include "core/hle/service/ldn/ldn_mitm_runtime.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <boost/asio.hpp>

namespace Service::LDN::Mitm {
namespace {
using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using Address = boost::asio::ip::address_v4;
using Error = boost::system::error_code;
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t NodeSize = 0x40;
constexpr std::size_t MaxPeers = 8;
constexpr std::size_t MaxQueuedFrames = 16;
constexpr std::size_t MaxPendingCommands = 64;

std::uint16_t Read16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}
IPv4 NodeAddress(std::span<const std::uint8_t> node) {
    return {node[3], node[2], node[1], node[0]};
}
} // namespace

bool ValidateNode(std::span<const std::uint8_t> node, std::string& error) {
    error.clear();
    if (node.size() != NodeSize || node[0xa] >= MaxPeers || node[0xb] > 1) {
        error = "invalid node record";
        return false;
    }
    return true;
}

bool ValidateNetwork(std::span<const std::uint8_t> network, std::string& error) {
    error.clear();
    if (network.size() != PayloadSizeMax || network[0x26] > 32 ||
        network[0x66] == 0 || network[0x66] > MaxPeers || network[0x67] == 0 ||
        network[0x67] > network[0x66] || Read16(network, 0x26a) > 0x180) {
        error = "invalid network record bounds";
        return false;
    }
    std::size_t connected = 0;
    for (std::size_t slot = 0; slot < MaxPeers; ++slot) {
        const auto node = network.subspan(0x68 + slot * NodeSize, NodeSize);
        if (!ValidateNode(node, error)) {
            return false;
        }
        if (node[0xb]) {
            if (node[0xa] != slot) {
                error = "invalid connected node slot";
                return false;
            }
            ++connected;
        }
    }
    if (connected != network[0x67] || network[0x68 + 0xb] != 1) {
        error = "network membership count disagrees with connected nodes";
        return false;
    }
    return true;
}

struct Runtime::Impl {
    struct State;
    mutable std::mutex lifecycle_mutex;
    mutable std::mutex status_mutex;
    std::mutex post_mutex;
    std::mutex receiver_mutex;
    Status status;
    const void* owner{};
    Receiver receiver;
    std::shared_ptr<State> state;
    std::shared_ptr<State> commands; // protected by post_mutex; cleared before joining
    std::thread thread;
    struct Command {
        int key; // -1 ordinary; 0 latest network; 1 leave; 2..9 close a specific slot
        std::function<void(State&)> execute;
    };
    std::deque<Command> pending; // at most 64 ordinary + 10 coalesced lifecycle commands
    bool dispatch_pending{}; // protected by post_mutex

    void ReportError(std::string error, bool malformed = false) {
        std::scoped_lock lock{status_mutex};
        status.last_error = std::move(error);
        status.malformed += malformed;
    }
    void Count(bool sent) {
        std::scoped_lock lock{status_mutex};
        if (sent) {
            ++status.sent;
        } else {
            ++status.received;
        }
    }
    void Deliver(Event event) {
        Receiver target;
        {
            std::scoped_lock lock{receiver_mutex};
            target = receiver;
        }
        if (target) {
            target(event);
        }
    }
    bool Post(std::function<void(State&)> command, std::string& error, int key = -1);
    void Drain(const std::shared_ptr<State>& s);
    void StopLocked();
};

struct Runtime::Impl::State : std::enable_shared_from_this<Runtime::Impl::State> {
    struct Peer : std::enable_shared_from_this<Peer> {
        std::weak_ptr<State> state;
        tcp::socket socket;
        boost::asio::steady_timer deadline;
        std::uint8_t slot;
        IPv4 address;
        std::uint64_t connection;
        bool joined{};
        bool closed{};
        StreamDecoder decoder;
        std::array<std::uint8_t, FrameSizeMax> input{};
        std::deque<std::shared_ptr<Bytes>> output;

        Peer(const std::shared_ptr<State>& state_, tcp::socket socket_, std::uint8_t slot_,
             IPv4 address_)
            : state{state_}, socket{std::move(socket_)}, deadline{socket.get_executor()},
              slot{slot_}, address{address_}, connection{state_->next_connection++} {}

        void ArmDeadline() {
            deadline.expires_after(std::chrono::seconds{1});
            deadline.async_wait([self = shared_from_this()](Error error) {
                if (!error && !self->closed) {
                    if (auto s = self->state.lock()) {
                        s->parent.ReportError("LDN TCP handshake timed out");
                        s->Remove(self, true);
                    }
                }
            });
        }
        void Read() {
            socket.async_read_some(boost::asio::buffer(input),
                                   [self = shared_from_this()](Error error, std::size_t count) {
                auto s = self->state.lock();
                if (!s || self->closed) {
                    return;
                }
                if (error) {
                    if (self->decoder.BufferedSize() != 0) {
                        s->parent.ReportError("truncated LDN TCP frame at EOF", true);
                    } else if (error != boost::asio::error::eof &&
                               error != boost::asio::error::operation_aborted) {
                        s->parent.ReportError("LDN TCP receive: " + error.message());
                    }
                    s->Remove(self, true);
                    return;
                }
                std::vector<Message> messages;
                std::string reason;
                const bool valid = self->decoder.Feed(std::span{self->input}.first(count),
                                                     messages, reason);
                for (auto& message : messages) {
                    if (!s->Receive(self, std::move(message))) {
                        return;
                    }
                }
                if (!valid) {
                    s->parent.ReportError("LDN TCP frame: " + reason, true);
                    s->Remove(self, true);
                    return;
                }
                if (!self->closed) {
                    self->Read();
                }
            });
        }
        void Enqueue(std::shared_ptr<Bytes> frame) {
            if (closed) {
                return;
            }
            if (output.size() >= MaxQueuedFrames) {
                if (auto s = state.lock()) {
                    s->parent.ReportError("LDN TCP send queue full");
                    s->Remove(shared_from_this(), true);
                }
                return;
            }
            const bool idle = output.empty();
            output.push_back(std::move(frame));
            if (idle) {
                Write();
            }
        }
        void Write() {
            // The frame's separate shared ownership keeps the outstanding buffer alive on Close.
            const auto frame = output.front();
            boost::asio::async_write(socket, boost::asio::buffer(*frame),
                                     [self = shared_from_this(), frame](Error error, std::size_t) {
                auto s = self->state.lock();
                if (!s || self->closed) {
                    return;
                }
                if (error) {
                    s->parent.ReportError("LDN TCP send: " + error.message());
                    s->Remove(self, true);
                    return;
                }
                s->parent.Count(true);
                self->output.pop_front();
                if (!self->output.empty()) {
                    self->Write();
                }
            });
        }
        void Close() {
            closed = true;
            deadline.cancel();
            Error ignored;
            socket.cancel(ignored);
            socket.shutdown(tcp::socket::shutdown_both, ignored);
            socket.close(ignored);
            output.clear();
        }
    };

    Impl& parent;
    Configuration config;
    boost::asio::io_context io;
    udp::socket unicast{io};
    udp::socket broadcast{io};
    tcp::acceptor listener{io};
    std::array<std::shared_ptr<Peer>, MaxPeers> peers{};
    std::optional<Bytes> network;
    std::string role{"idle"};
    bool closing{};
    std::size_t udp_pending{};
    std::uint64_t next_connection{1};

    State(Impl& parent_, Configuration config_) : parent{parent_}, config{std::move(config_)} {}

    Address BroadcastAddress() const {
        return Address{Address{config.address}.to_uint() | ~Address{config.netmask}.to_uint()};
    }
    void Open() {
        unicast.open(udp::v4());
        unicast.set_option(udp::socket::reuse_address{true});
        unicast.set_option(boost::asio::socket_base::broadcast{true});
        unicast.bind(udp::endpoint{Address{config.address}, config.port});
#if !defined(_WIN32)
        // Linux does not deliver subnet broadcasts to a socket bound to a unicast address.
        broadcast.open(udp::v4());
        broadcast.set_option(udp::socket::reuse_address{true});
        broadcast.set_option(boost::asio::socket_base::broadcast{true});
        broadcast.bind(udp::endpoint{BroadcastAddress(), config.port});
#endif
        listener.open(tcp::v4());
#if defined(_WIN32)
        listener.set_option(
            boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_EXCLUSIVEADDRUSE>{true});
#else
        listener.set_option(tcp::acceptor::reuse_address{true});
#endif
        listener.bind(tcp::endpoint{Address{config.address}, config.port});
        listener.listen(7);
    }
    void Begin() {
        ReceiveUdp(unicast);
        if (broadcast.is_open()) {
            ReceiveUdp(broadcast);
        }
        Accept();
    }
    void UpdateStatus() {
        std::scoped_lock lock{parent.status_mutex};
        parent.status.role = role;
        parent.status.peers = std::count_if(peers.begin(), peers.end(),
                                           [](const auto& p) { return p && p->joined; });
    }
    void SendUdp(PacketType type, std::span<const std::uint8_t> payload, udp::endpoint target) {
        std::string error;
        auto encoded = Encode(type, payload, error);
        if (!encoded || udp_pending >= MaxQueuedFrames) {
            parent.ReportError(encoded ? "LDN UDP send queue full" : error);
            return;
        }
        ++udp_pending;
        auto frame = std::make_shared<Bytes>(std::move(*encoded));
        unicast.async_send_to(boost::asio::buffer(*frame), target,
                              [self = shared_from_this(), frame](Error ec, std::size_t) {
            --self->udp_pending;
            if (ec && !self->closing) {
                self->parent.ReportError("LDN UDP send: " + ec.message());
            } else if (!ec) {
                self->parent.Count(true);
            }
        });
    }
    void ReceiveUdp(udp::socket& socket) {
        struct Datagram {
            // One extra byte makes oversized UDP frames detectable without accepting a prefix.
            std::array<std::uint8_t, FrameSizeMax + 1> bytes{};
            udp::endpoint sender;
        };
        auto datagram = std::make_shared<Datagram>();
        socket.async_receive_from(boost::asio::buffer(datagram->bytes), datagram->sender,
                                  [self = shared_from_this(), datagram, socket_ptr = &socket]
                                  (Error ec, std::size_t count) {
            if (self->closing) {
                return;
            }
            if (ec) {
                if (ec != boost::asio::error::operation_aborted) {
                    self->parent.ReportError("LDN UDP receive: " + ec.message(),
                                             ec == boost::asio::error::message_size);
                }
            } else if (datagram->sender.address().is_v4() &&
                       datagram->sender.address().to_v4().to_bytes() != self->config.address) {
                std::string reason;
                auto message = Decode(std::span{datagram->bytes}.first(count), reason);
                if (!message || (message->type != PacketType::Scan &&
                                 message->type != PacketType::ScanResp)) {
                    self->parent.ReportError("LDN UDP frame: " +
                        (message ? std::string{"wrong packet type"} : reason), true);
                } else if (message->type == PacketType::Scan) {
                    self->parent.Count(false);
                    if (self->network) {
                        self->SendUdp(PacketType::ScanResp, *self->network, datagram->sender);
                    }
                } else if (!ValidateNetwork(message->payload, reason)) {
                    self->parent.ReportError("LDN scan response: " + reason, true);
                } else {
                    self->parent.Count(false);
                    self->parent.Deliver({Event::Kind::Message, 0,
                        datagram->sender.address().to_v4().to_bytes(), std::move(*message)});
                }
            }
            self->ReceiveUdp(*socket_ptr);
        });
    }
    void Accept() {
        listener.async_accept([self = shared_from_this()](Error error, tcp::socket socket) {
            if (self->closing) {
                return;
            }
            if (!error) {
                std::size_t slot = 1;
                const auto maximum = self->network ? (*self->network)[0x66] : 1;
                const auto reserved = std::count_if(self->peers.begin() + 1, self->peers.end(),
                                                    [](const auto& peer) { return bool(peer); });
                for (; slot < MaxPeers && self->peers[slot]; ++slot) {}
                if (slot < MaxPeers && reserved + 1 < maximum) {
                    Error endpoint_error;
                    const auto endpoint = socket.remote_endpoint(endpoint_error);
                    if (!endpoint_error && endpoint.address().is_v4()) {
                        socket.set_option(tcp::no_delay{true}, endpoint_error);
                        auto peer = std::make_shared<Peer>(self, std::move(socket),
                            static_cast<std::uint8_t>(slot), endpoint.address().to_v4().to_bytes());
                        self->peers[slot] = peer;
                        peer->ArmDeadline();
                        peer->Read();
                    }
                }
            } else if (error != boost::asio::error::operation_aborted) {
                self->parent.ReportError("LDN TCP accept: " + error.message());
            }
            self->Accept();
        });
    }
    bool Receive(const std::shared_ptr<Peer>& peer, Message message) {
        std::string reason;
        if (peer->slot != 0) {
            if (!network || message.type != PacketType::Connect || peer->joined ||
                !ValidateNode(message.payload, reason) ||
                NodeAddress(message.payload) != peer->address) {
                parent.ReportError("invalid LDN station handshake: " + reason, true);
                Remove(peer, true);
                return false;
            }
        } else if (message.type != PacketType::SyncNetwork ||
                   !ValidateNetwork(message.payload, reason) ||
                   NodeAddress(std::span{message.payload}.subspan(0x68, NodeSize)) != peer->address) {
            parent.ReportError("invalid LDN host synchronization: " + reason, true);
            Remove(peer, true);
            return false;
        }
        peer->joined = true;
        peer->deadline.cancel();
        if (peer->slot == 0) {
            role = "station";
        }
        UpdateStatus();
        parent.Count(false);
        parent.Deliver({Event::Kind::Message, peer->slot, peer->address, std::move(message),
                        peer->connection});
        return !peer->closed;
    }
    void Remove(const std::shared_ptr<Peer>& peer, bool notify) {
        if (peer->closed) {
            return;
        }
        // May be passed an element of peers; hold a copy before clearing the slot.
        auto keep = peer;
        keep->Close();
        if (peers[keep->slot] == keep) {
            peers[keep->slot].reset();
        }
        if (keep->slot == 0) {
            role = "idle";
        }
        UpdateStatus();
        if (notify && (keep->joined || keep->slot == 0)) {
            parent.Deliver({Event::Kind::PeerClosed, keep->slot, keep->address, {},
                            keep->connection});
        }
    }
    void Leave() {
        for (auto& peer : peers) {
            if (peer) {
                Remove(peer, false);
            }
        }
        network.reset();
        role = "idle";
        UpdateStatus();
    }
    void Publish(std::optional<Bytes> value) {
        if (!value) {
            Leave();
            return;
        }
        if (!network) {
            Leave();
        }
        network = std::move(value);
        role = "host";
        UpdateStatus();
        std::string error;
        auto encoded = Encode(PacketType::SyncNetwork, *network, error);
        auto frame = std::make_shared<Bytes>(std::move(*encoded));
        for (auto& peer : peers) {
            if (peer && peer->slot != 0 && peer->joined) {
                peer->Enqueue(frame);
            }
        }
    }
    void Connect(IPv4 host, Bytes node) {
        Leave();
        role = "connecting";
        UpdateStatus();
        auto peer = std::make_shared<Peer>(shared_from_this(), tcp::socket{io},
            std::uint8_t{0}, host);
        peers[0] = peer;
        Error error;
        peer->socket.open(tcp::v4(), error);
        if (!error) {
            peer->socket.bind(tcp::endpoint{Address{config.address}, 0}, error);
        }
        if (error) {
            parent.ReportError("LDN TCP bind: " + error.message());
            Remove(peer, true);
            return;
        }
        peer->ArmDeadline();
        peer->socket.async_connect(tcp::endpoint{Address{host}, config.port},
                                   [self = shared_from_this(), peer, node = std::move(node)]
                                   (Error ec) {
            if (peer->closed) {
                return;
            }
            if (ec) {
                self->parent.ReportError("LDN TCP connect: " + ec.message());
                self->Remove(peer, true);
                return;
            }
            peer->socket.set_option(tcp::no_delay{true}, ec);
            std::string reason;
            auto encoded = Encode(PacketType::Connect, node, reason);
            peer->Enqueue(std::make_shared<Bytes>(std::move(*encoded)));
            peer->Read();
        });
    }
    void Shutdown() {
        closing = true;
        Leave();
        Error ignored;
        listener.close(ignored);
        unicast.close(ignored);
        broadcast.close(ignored);
        // Let run() drain cancellations; stopping io_context here would retain handler cycles.
    }
};

bool Runtime::Impl::Post(std::function<void(State&)> command, std::string& error, int key) {
    std::scoped_lock posting{post_mutex};
    error.clear();
    if (!commands) {
        error = "LDN LAN transport is stopped";
    } else if (key < 0 && static_cast<std::size_t>(std::count_if(pending.begin(), pending.end(),
               [](const Command& c) { return c.key < 0; })) >= MaxPendingCommands) {
        error = "LDN LAN command queue is full";
    } else {
        // Coalesced lifecycle operations have reserved capacity. Move a replacement
        // to the tail so the latest intent retains its order relative to other commands.
        if (key >= 0) {
            std::erase_if(pending, [key](const Command& c) { return c.key == key; });
        }
        pending.push_back({key, std::move(command)});
        {
            std::scoped_lock lock{status_mutex};
            status.pending_commands = pending.size();
        }
        if (!dispatch_pending) {
            dispatch_pending = true;
            boost::asio::post(commands->io, [this, s = commands] { Drain(s); });
        }
        return true;
    }
    ReportError(error);
    return false;
}

void Runtime::Impl::Drain(const std::shared_ptr<State>& s) {
    std::optional<Command> command;
    {
        std::scoped_lock posting{post_mutex};
        if (pending.empty()) {
            dispatch_pending = false;
            return;
        }
        command = std::move(pending.front());
        pending.pop_front();
        std::scoped_lock lock{status_mutex};
        status.pending_commands = pending.size();
    }
    command->execute(*s);
    {
        std::scoped_lock posting{post_mutex};
        if (!pending.empty()) {
            boost::asio::post(s->io, [this, s] { Drain(s); });
        } else {
            dispatch_pending = false;
        }
    }
}

void Runtime::Impl::StopLocked() {
    if (!state) {
        return;
    }
    {
        std::scoped_lock posting{post_mutex};
        commands.reset();
        pending.clear();
        {
            std::scoped_lock lock{status_mutex};
            status.pending_commands = 0;
        }
        boost::asio::post(state->io, [s = state] { s->Shutdown(); });
    }
    thread.join();
    state.reset();
    {
        std::scoped_lock lock{status_mutex};
        status.running = false;
        status.role = "idle";
        status.peers = 0;
    }
    Deliver({Event::Kind::Stopped, 0, {}, {}});
}

Runtime::Runtime() : impl{std::make_unique<Impl>()} {}
Runtime::~Runtime() {
    Stop();
}

bool Runtime::Start(const Configuration& configuration, std::string& error) {
    std::scoped_lock lifecycle{impl->lifecycle_mutex};
    error.clear();
    if (impl->state) {
        if (impl->state->config == configuration) {
            return true;
        }
        impl->StopLocked();
    }
    auto state = std::make_shared<Impl::State>(*impl, configuration);
    try {
        state->Open();
        {
            std::scoped_lock lock{impl->status_mutex};
            impl->status = {};
            impl->status.running = true;
            impl->status.configuration = configuration;
        }
        impl->state = state;
        state->Begin();
        impl->thread = std::thread{[state] { state->io.run(); }};
        {
            std::scoped_lock posting{impl->post_mutex};
            impl->commands = state;
        }
        return true;
    } catch (const std::exception& exception) {
        error = "cannot start LDN LAN on " + Address{configuration.address}.to_string() + ":" +
                std::to_string(configuration.port) + ": " + exception.what();
        if (impl->state) {
            // Thread creation failed after async operations were registered.
            state->Shutdown();
            state->io.run();
            impl->state.reset();
        }
        {
            std::scoped_lock lock{impl->status_mutex};
            impl->status.running = false;
            impl->status.configuration = configuration;
            impl->status.last_error = error;
        }
        return false;
    }
}

void Runtime::Stop() {
    std::scoped_lock lifecycle{impl->lifecycle_mutex};
    impl->StopLocked();
}
Status Runtime::GetStatus() const {
    std::scoped_lock lock{impl->status_mutex};
    return impl->status;
}
bool Runtime::Attach(const void* owner, Receiver receiver, std::string& error) {
    std::scoped_lock lifecycle{impl->lifecycle_mutex};
    std::scoped_lock lock{impl->receiver_mutex};
    error.clear();
    if (impl->owner && impl->owner != owner) {
        error = "another LDN service owns the LAN session";
        return false;
    }
    impl->owner = owner;
    impl->receiver = std::move(receiver);
    return true;
}
void Runtime::Detach(const void* owner) {
    std::scoped_lock lifecycle{impl->lifecycle_mutex};
    {
        std::scoped_lock lock{impl->receiver_mutex};
        if (impl->owner != owner) {
            return;
        }
    }
    impl->StopLocked();
    std::scoped_lock lock{impl->receiver_mutex};
    impl->receiver = {};
    impl->owner = nullptr;
}
bool Runtime::Scan(std::string& error) {
    return impl->Post([](Impl::State& s) {
        s.SendUdp(PacketType::Scan, {}, udp::endpoint{s.BroadcastAddress(), s.config.port});
    }, error);
}
bool Runtime::Connect(IPv4 host, Bytes node, std::string& error) {
    if (!ValidateNode(node, error)) { return false; }
    return impl->Post([host, node = std::move(node)](Impl::State& s) mutable {
        s.Connect(host, std::move(node));
    }, error);
}
void Runtime::Leave() {
    std::string error;
    impl->Post([](Impl::State& s) { s.Leave(); }, error, 1);
}
void Runtime::PublishNetwork(std::optional<Bytes> network) {
    if (!network) { Leave(); return; }
    std::string error;
    if (network && !ValidateNetwork(*network, error)) {
        impl->ReportError("cannot publish LDN network: " + error);
        return;
    }
    impl->Post([value = std::move(network)](Impl::State& s) mutable {
        s.Publish(std::move(value));
    }, error, 0);
}
void Runtime::ClosePeer(std::uint8_t slot, std::uint64_t connection) {
    if (slot >= MaxPeers) { return; }
    std::string error;
    impl->Post([slot, connection](Impl::State& s) {
        if (s.peers[slot] && (!connection || s.peers[slot]->connection == connection)) {
            s.Remove(s.peers[slot], true);
        }
    }, error, 2 + slot);
}
} // namespace Service::LDN::Mitm
