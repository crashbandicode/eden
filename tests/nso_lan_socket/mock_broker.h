// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A mock LAN broker over real Unix sockets, for exercising Network::NsoLanSocket.
//
// Each emulated instance gets a newline-JSON control socket (AF_UNIX stream) and a datagram
// endpoint (AF_UNIX dgram). All instances share one (virtual IP, guest port) -> socket-path
// registry, so datagrams really travel guest A -> mock -> guest B over the kernel. The frame
// encoder/decoder here is written from the wire description with plain index arithmetic and
// shares no code with the production codec: it is the independent oracle.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace Mock {

using Bytes = std::vector<std::uint8_t>;
using Ip = std::array<std::uint8_t, 4>;

struct OracleFrame {
    Ip source;
    Ip destination;
    unsigned source_port;
    unsigned destination_port;
    Bytes payload;
};

inline Bytes OracleEncode(Ip source, Ip destination, unsigned source_port,
                          unsigned destination_port, const Bytes& payload) {
    Bytes out{'E', 'L', 'N', '1', 1, 0, 0, 0};
    out.insert(out.end(), source.begin(), source.end());
    out.insert(out.end(), destination.begin(), destination.end());
    out.push_back(static_cast<std::uint8_t>(source_port >> 8));
    out.push_back(static_cast<std::uint8_t>(source_port & 0xff));
    out.push_back(static_cast<std::uint8_t>(destination_port >> 8));
    out.push_back(static_cast<std::uint8_t>(destination_port & 0xff));
    const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
    out.push_back(static_cast<std::uint8_t>(length >> 24));
    out.push_back(static_cast<std::uint8_t>((length >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((length >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(length & 0xff));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

inline std::optional<OracleFrame> OracleDecode(const Bytes& b) {
    if (b.size() < 24 || std::memcmp(b.data(), "ELN1", 4) != 0 || b[4] != 1 || b[5] != 0 ||
        b[6] != 0 || b[7] != 0) {
        return std::nullopt;
    }
    const std::uint32_t length = static_cast<std::uint32_t>(b[20]) << 24 |
                                 static_cast<std::uint32_t>(b[21]) << 16 |
                                 static_cast<std::uint32_t>(b[22]) << 8 | b[23];
    // 24 + 65483 = 65507, the largest IPv4 UDP payload: the whole frame must fit one datagram.
    if (length > 65483 || b.size() != 24 + static_cast<std::size_t>(length)) {
        return std::nullopt;
    }
    OracleFrame frame;
    std::copy(b.begin() + 8, b.begin() + 12, frame.source.begin());
    std::copy(b.begin() + 12, b.begin() + 16, frame.destination.begin());
    frame.source_port = static_cast<unsigned>(b[16]) << 8 | b[17];
    frame.destination_port = static_cast<unsigned>(b[18]) << 8 | b[19];
    frame.payload.assign(b.begin() + 24, b.end());
    return frame;
}

struct Request {
    std::size_t instance;
    std::string line; ///< Exactly what the guest sent, without the terminating newline.
};

struct Frame {
    std::size_t instance; ///< Instance whose datagram endpoint received the bytes.
    Bytes bytes;
};

class Broker {
public:
    explicit Broker(std::string directory) : directory_{std::move(directory)} {}
    ~Broker() {
        Stop();
    }

    // Instances must be added before Start().
    std::size_t AddInstance(Ip address) {
        const std::size_t index = instances_.size();
        Instance instance;
        instance.address = address;
        instance.control_path = directory_ + "/control-" + std::to_string(index) + ".sock";
        instance.datagram_path = directory_ + "/data-" + std::to_string(index) + ".sock";
        instances_.push_back(std::move(instance));
        return index;
    }

    const std::string& ControlPath(std::size_t i) const {
        return instances_.at(i).control_path;
    }
    const std::string& DatagramPath(std::size_t i) const {
        return instances_.at(i).datagram_path;
    }
    Ip Address(std::size_t i) const {
        return instances_.at(i).address;
    }
    const std::string& Directory() const {
        return directory_;
    }

    bool Start() {
        if (::pipe2(wake_, O_NONBLOCK | O_CLOEXEC) != 0) {
            return false;
        }
        for (auto& instance : instances_) {
            instance.listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            instance.datagram = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (instance.listener < 0 || instance.datagram < 0 ||
                !BindTo(instance.listener, instance.control_path) ||
                ::listen(instance.listener, 16) != 0 ||
                !BindTo(instance.datagram, instance.datagram_path)) {
                return false;
            }
            const int buffer = 4 * 1024 * 1024;
            (void)::setsockopt(instance.datagram, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
        }
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    void Stop() {
        if (thread_.joinable()) {
            stop_ = true;
            const char byte = 1;
            (void)::write(wake_[1], &byte, 1);
            thread_.join();
        }
        for (auto& instance : instances_) {
            CloseFd(instance.listener);
            CloseFd(instance.datagram);
            (void)::unlink(instance.control_path.c_str());
            (void)::unlink(instance.datagram_path.c_str());
        }
        CloseFd(wake_[0]);
        CloseFd(wake_[1]);
    }

    // The next control request to `instance` is answered with exactly these bytes (and the
    // connection is then closed), instead of by the registry logic. Still recorded.
    void ScriptReply(std::size_t instance, std::string raw) {
        std::lock_guard lock{mutex_};
        instances_.at(instance).scripts.push_back({false, std::move(raw)});
    }
    // The next control request is read and then never answered.
    void ScriptHang(std::size_t instance) {
        std::lock_guard lock{mutex_};
        instances_.at(instance).scripts.push_back({true, {}});
    }
    void SetRouting(bool enabled) {
        std::lock_guard lock{mutex_};
        routing_ = enabled;
    }

    std::vector<Request> Requests() const {
        std::lock_guard lock{mutex_};
        return requests_;
    }
    std::vector<Frame> Frames() const {
        std::lock_guard lock{mutex_};
        return frames_;
    }
    std::size_t FramesHandled() const {
        std::lock_guard lock{mutex_};
        return handled_;
    }
    std::size_t Unroutable() const {
        std::lock_guard lock{mutex_};
        return unroutable_;
    }
    // Requests or frames that did not match the documented protocol. Must stay zero.
    std::vector<std::string> Violations() const {
        std::lock_guard lock{mutex_};
        return violations_;
    }
    bool IsRegistered(Ip address, unsigned port) const {
        std::lock_guard lock{mutex_};
        return registry_.contains({address, port});
    }

    bool WaitForRequests(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [&] { return requests_.size() >= count; });
    }
    // Waits until `count` datagrams were received AND their forwarding attempt has finished.
    bool WaitForFramesHandled(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [&] { return handled_ >= count; });
    }

    // Puts raw bytes straight into a guest socket's receive queue. Synchronous: when this
    // returns 0 the datagram is queued. Returns errno on failure.
    static int Inject(const std::string& socket_path, const Bytes& bytes) {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size());
        const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return errno;
        }
        const ssize_t sent = ::sendto(fd, bytes.data(), bytes.size(), 0,
                                      reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        const int error = sent == static_cast<ssize_t>(bytes.size()) ? 0 : errno;
        (void)::close(fd);
        return error;
    }

private:
    struct Script {
        bool hang;
        std::string raw;
    };
    struct Instance {
        std::string control_path;
        std::string datagram_path;
        Ip address{};
        int listener = -1;
        int datagram = -1;
        unsigned next_ephemeral = 49152;
        std::deque<Script> scripts;
    };
    struct Connection {
        int fd;
        std::size_t instance;
        std::string buffer;
        bool hung = false;
    };
    enum class Kind { Wake, Listener, Datagram, Connection };
    struct Reply {
        bool hang = false;
        std::string raw;
    };

    static void CloseFd(int& fd) {
        if (fd >= 0) {
            (void)::close(fd);
            fd = -1;
        }
    }

    static bool BindTo(int fd, const std::string& path) {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) {
            return false;
        }
        std::memcpy(address.sun_path, path.c_str(), path.size());
        return ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }

    void Run() {
        std::vector<Connection> connections;
        while (!stop_) {
            std::vector<pollfd> fds;
            std::vector<std::pair<Kind, std::size_t>> kinds;
            fds.push_back({wake_[0], POLLIN, 0});
            kinds.push_back({Kind::Wake, 0});
            for (std::size_t i = 0; i < instances_.size(); ++i) {
                fds.push_back({instances_[i].listener, POLLIN, 0});
                kinds.push_back({Kind::Listener, i});
                fds.push_back({instances_[i].datagram, POLLIN, 0});
                kinds.push_back({Kind::Datagram, i});
            }
            for (std::size_t i = 0; i < connections.size(); ++i) {
                if (!connections[i].hung) {
                    fds.push_back({connections[i].fd, POLLIN, 0});
                    kinds.push_back({Kind::Connection, i});
                }
            }
            if (::poll(fds.data(), fds.size(), -1) < 0 && errno != EINTR) {
                break;
            }
            std::vector<std::size_t> closed;
            for (std::size_t n = 0; n < fds.size(); ++n) {
                if (fds[n].revents == 0) {
                    continue;
                }
                const auto [kind, index] = kinds[n];
                switch (kind) {
                case Kind::Wake:
                    break;
                case Kind::Listener:
                    AcceptAll(index, connections);
                    break;
                case Kind::Datagram:
                    DrainDatagrams(index);
                    break;
                case Kind::Connection:
                    if (!ServeConnection(connections[index])) {
                        closed.push_back(index);
                    }
                    break;
                }
            }
            std::sort(closed.rbegin(), closed.rend());
            for (const std::size_t index : closed) {
                (void)::close(connections[index].fd);
                connections.erase(connections.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }
        for (auto& connection : connections) {
            (void)::close(connection.fd);
        }
    }

    void AcceptAll(std::size_t instance, std::vector<Connection>& connections) {
        while (true) {
            const int fd = ::accept4(instances_[instance].listener, nullptr, nullptr,
                                     SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                return;
            }
            connections.push_back({fd, instance, {}, false});
        }
    }

    // Returns false when the connection should be closed.
    bool ServeConnection(Connection& connection) {
        char chunk[4096];
        const ssize_t n = ::recv(connection.fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return false;
        }
        connection.buffer.append(chunk, static_cast<std::size_t>(n));
        const auto newline = connection.buffer.find('\n');
        if (newline == std::string::npos) {
            return connection.buffer.size() < 65536;
        }
        const Reply reply = HandleRequest(connection.instance, connection.buffer.substr(0, newline));
        if (reply.hang) {
            connection.hung = true;
            return true;
        }
        std::size_t sent = 0;
        while (sent < reply.raw.size()) {
            const ssize_t w = ::send(connection.fd, reply.raw.data() + sent, reply.raw.size() - sent,
                                     MSG_NOSIGNAL);
            if (w <= 0) {
                break;
            }
            sent += static_cast<std::size_t>(w);
        }
        return false;
    }

    Reply HandleRequest(std::size_t instance_index, const std::string& line) {
        std::lock_guard lock{mutex_};
        requests_.push_back({instance_index, line});
        changed_.notify_all();
        Instance& instance = instances_[instance_index];
        if (!instance.scripts.empty()) {
            Script script = std::move(instance.scripts.front());
            instance.scripts.pop_front();
            return {script.hang, std::move(script.raw)};
        }

        const auto fail = [&](const std::string& why) {
            violations_.push_back(why + ": " + line);
            return Reply{false, "{\"ok\":false,\"error\":\"bad request\"}\n"};
        };
        const nlohmann::json body = nlohmann::json::parse(line, nullptr, false);
        if (!body.is_object() || !body.contains("op") || !body["op"].is_string() ||
            !body.contains("socket") || !body["socket"].is_string()) {
            return fail("not an op request");
        }
        const std::string op = body["op"];
        const std::string socket = body["socket"];
        // The guest's own socket must live in the broker directory and look like what it makes.
        if (socket.rfind(directory_ + "/eln-", 0) != 0 || socket.find('/', directory_.size() + 1) !=
                                                              std::string::npos) {
            return fail("socket outside the broker directory");
        }
        if (op == "bind") {
            if (body.size() != 3 || !body.contains("port") || !body["port"].is_number_unsigned() ||
                body["port"].get<unsigned long long>() > 65535) {
                return fail("bind keys");
            }
            unsigned port = body["port"].get<unsigned>();
            if (port == 0) {
                while (registry_.contains({instance.address, instance.next_ephemeral})) {
                    ++instance.next_ephemeral;
                }
                port = instance.next_ephemeral++;
            } else if (registry_.contains({instance.address, port})) {
                return {false, "{\"ok\":false,\"error\":\"port_in_use\"}\n"};
            }
            registry_[{instance.address, port}] = socket;
            return {false, "{\"ok\":true,\"port\":" + std::to_string(port) + "}\n"};
        }
        if (op == "unbind") {
            if (body.size() != 2) {
                return fail("unbind keys");
            }
            for (auto it = registry_.begin(); it != registry_.end();) {
                it = it->second == socket && it->first.first == instance.address
                         ? registry_.erase(it)
                         : std::next(it);
            }
            return {false, "{\"ok\":true}\n"};
        }
        return fail("unknown op");
    }

    void DrainDatagrams(std::size_t instance_index) {
        static thread_local Bytes buffer(70000);
        while (true) {
            const ssize_t n = ::recv(instances_[instance_index].datagram, buffer.data(),
                                     buffer.size(), MSG_DONTWAIT);
            if (n < 0) {
                return;
            }
            Bytes bytes(buffer.begin(), buffer.begin() + n);
            std::lock_guard lock{mutex_};
            Route(instance_index, bytes);
            frames_.push_back({instance_index, std::move(bytes)});
            ++handled_;
            changed_.notify_all();
        }
    }

    // Caller holds mutex_.
    void Route(std::size_t instance_index, const Bytes& bytes) {
        const auto frame = OracleDecode(bytes);
        if (!frame) {
            violations_.push_back("guest sent an invalid frame");
            return;
        }
        if (frame->source != instances_[instance_index].address) {
            violations_.push_back("guest spoofed its source address");
            return;
        }
        const auto it = registry_.find({frame->destination, frame->destination_port});
        if (it == registry_.end() || !routing_) {
            ++unroutable_;
            return;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, it->second.c_str(), it->second.size());
        // Deliver the very same bytes: the header already describes the original src/dst.
        if (::sendto(instances_[instance_index].datagram, bytes.data(), bytes.size(),
                     MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)) != static_cast<ssize_t>(bytes.size())) {
            ++unroutable_;
        }
    }

    std::string directory_;
    std::vector<Instance> instances_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    int wake_[2] = {-1, -1};

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Request> requests_;
    std::vector<Frame> frames_;
    std::vector<std::string> violations_;
    std::map<std::pair<Ip, unsigned>, std::string> registry_;
    std::size_t handled_ = 0;
    std::size_t unroutable_ = 0;
    bool routing_ = true;
};

} // namespace Mock
