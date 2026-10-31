// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A mock LAN broker over real Windows AF_UNIX stream sockets, for exercising the Windows
// Network::NsoLanSocket (nso_lan_socket_win.cpp). Each emulated instance gets a newline-JSON
// control listener (control-<i>.sock) and a data listener (data-<i>.sock). A data connection
// introduces itself with a hello line naming the socket node the guest created; afterwards it
// carries consecutive ELN1 records. All instances share one (virtual IP, guest port) -> socket
// path registry, so records really travel guest A -> mock -> guest B over the kernel. The frame
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

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <afunix.h>

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

struct Hello {
    std::size_t instance;
    std::string line;
};

struct Frame {
    std::size_t instance; ///< Instance whose data endpoint received the record.
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
        instance.control_path = directory_ + "\\control-" + std::to_string(index) + ".sock";
        instance.datagram_path = directory_ + "\\data-" + std::to_string(index) + ".sock";
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
        for (auto& instance : instances_) {
            instance.listener = Listen(instance.control_path);
            instance.data_listener = Listen(instance.datagram_path);
            if (instance.listener == INVALID_SOCKET || instance.data_listener == INVALID_SOCKET) {
                return false;
            }
        }
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    void Stop() {
        if (thread_.joinable()) {
            stop_ = true;
            thread_.join();
        }
        for (auto& instance : instances_) {
            CloseSocket(instance.listener);
            CloseSocket(instance.data_listener);
            DeleteNode(instance.control_path);
            DeleteNode(instance.datagram_path);
        }
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
    // The next data hello to `instance` is refused with this error code.
    void ScriptHelloRefusal(std::size_t instance, std::string code) {
        std::lock_guard lock{mutex_};
        instances_.at(instance).hello_refusals.push_back(std::move(code));
    }
    void PauseDataReads(bool paused) { data_reads_paused_ = paused; }
    bool DataReadsPaused() const { return data_read_pause_observed_.load(); }

    void SetRouting(bool enabled) {
        std::lock_guard lock{mutex_};
        routing_ = enabled;
    }

    std::vector<Request> Requests() const {
        std::lock_guard lock{mutex_};
        return requests_;
    }
    std::vector<Hello> Hellos() const {
        std::lock_guard lock{mutex_};
        return hellos_;
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
    // Requests, hellos or records that did not match the documented protocol. Must stay zero.
    std::vector<std::string> Violations() const {
        std::lock_guard lock{mutex_};
        return violations_;
    }
    bool IsRegistered(Ip address, unsigned port) const {
        std::lock_guard lock{mutex_};
        return registry_.contains({address, port});
    }
    bool HasStream(const std::string& path) const {
        std::lock_guard lock{mutex_};
        return streams_.contains(path);
    }

    bool WaitForRequests(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [&] { return requests_.size() >= count; });
    }
    bool WaitForHellos(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [&] { return hellos_.size() >= count; });
    }
    // Waits until `count` records were received AND their forwarding attempt has finished.
    bool WaitForFramesHandled(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [&] { return handled_ >= count; });
    }

    // Puts raw bytes straight onto a guest socket's stream. Synchronous: when this returns 0 the
    // bytes are in the kernel. Returns a Winsock error code (or -1 when the stream is unknown).
    int Inject(const std::string& socket_path, const Bytes& bytes) {
        SOCKET s;
        {
            std::lock_guard lock{mutex_};
            const auto it = streams_.find(socket_path);
            if (it == streams_.end()) {
                return -1;
            }
            s = it->second;
        }
        return SendAll(s, bytes);
    }

    // The broker side closes a guest's data stream (as a crashed broker would).
    bool CloseStream(const std::string& socket_path) {
        std::lock_guard lock{mutex_};
        const auto it = streams_.find(socket_path);
        if (it == streams_.end()) {
            return false;
        }
        close_requests_.push_back(it->second);
        return true;
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
        SOCKET listener = INVALID_SOCKET;
        SOCKET data_listener = INVALID_SOCKET;
        unsigned next_ephemeral = 49152;
        std::deque<Script> scripts;
        std::deque<std::string> hello_refusals;
    };
    enum class Kind { Control, DataPending, DataClient };
    struct Connection {
        SOCKET s;
        std::size_t instance;
        Kind kind;
        std::string buffer;
        std::string path; ///< DataClient: the guest node this stream introduced itself with.
        bool hung = false;
    };
    struct Reply {
        bool hang = false;
        std::string raw;
    };

    static void CloseSocket(SOCKET& s) {
        if (s != INVALID_SOCKET) {
            (void)::closesocket(s);
            s = INVALID_SOCKET;
        }
    }

    static void DeleteNode(const std::string& path) {
        std::wstring wide(path.begin(), path.end());
        (void)::DeleteFileW(wide.c_str());
    }

    static bool SetNonBlocking(SOCKET s) {
        u_long mode = 1;
        return ::ioctlsocket(s, FIONBIO, &mode) == 0;
    }

    static SOCKET Listen(const std::string& path) {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) {
            return INVALID_SOCKET;
        }
        std::memcpy(address.sun_path, path.c_str(), path.size());
        DeleteNode(path);
        SOCKET s = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) {
            return INVALID_SOCKET;
        }
        if (::bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(s, 16) != 0 || !SetNonBlocking(s)) {
            ::closesocket(s);
            return INVALID_SOCKET;
        }
        return s;
    }

    static int SendAll(SOCKET s, const Bytes& bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const int n = ::send(s, reinterpret_cast<const char*>(bytes.data() + sent),
                                 static_cast<int>(bytes.size() - sent), 0);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            const int e = ::WSAGetLastError();
            if (e == WSAEWOULDBLOCK) {
                WSAPOLLFD entry{s, POLLWRNORM, 0};
                if (::WSAPoll(&entry, 1, 2000) <= 0) {
                    return WSAETIMEDOUT;
                }
                continue;
            }
            return e;
        }
        return 0;
    }

    void Run() {
        std::vector<Connection> connections;
        while (!stop_) {
            std::vector<WSAPOLLFD> fds;
            std::vector<std::pair<int, std::size_t>> kinds; // 0 control listener, 1 data listener, 2 connection
            for (std::size_t i = 0; i < instances_.size(); ++i) {
                fds.push_back({instances_[i].listener, POLLRDNORM, 0});
                kinds.push_back({0, i});
                fds.push_back({instances_[i].data_listener, POLLRDNORM, 0});
                kinds.push_back({1, i});
            }
            const bool pause_data = data_reads_paused_.load();
            data_read_pause_observed_ = pause_data;
            for (std::size_t i = 0; i < connections.size(); ++i) {
                if (!connections[i].hung && !(pause_data && connections[i].kind == Kind::DataClient)) {
                    fds.push_back({connections[i].s, POLLRDNORM, 0});
                    kinds.push_back({2, i});
                }
            }
            const int ready = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), 50);
            std::vector<std::size_t> closed;
            if (ready > 0) {
                for (std::size_t n = 0; n < fds.size(); ++n) {
                    if (fds[n].revents == 0) {
                        continue;
                    }
                    const auto [kind, index] = kinds[n];
                    switch (kind) {
                    case 0:
                        AcceptAll(instances_[index].listener, index, Kind::Control, connections);
                        break;
                    case 1:
                        AcceptAll(instances_[index].data_listener, index, Kind::DataPending, connections);
                        break;
                    default:
                        if (!Serve(connections[index])) {
                            closed.push_back(index);
                        }
                        break;
                    }
                }
            }
            {
                std::lock_guard lock{mutex_};
                for (const SOCKET s : close_requests_) {
                    for (std::size_t i = 0; i < connections.size(); ++i) {
                        if (connections[i].s == s && std::find(closed.begin(), closed.end(), i) == closed.end()) {
                            closed.push_back(i);
                        }
                    }
                }
                close_requests_.clear();
            }
            std::sort(closed.begin(), closed.end());
            closed.erase(std::unique(closed.begin(), closed.end()), closed.end());
            for (auto it = closed.rbegin(); it != closed.rend(); ++it) {
                Connection& c = connections[*it];
                if (c.kind == Kind::DataClient) {
                    std::lock_guard lock{mutex_};
                    if (auto found = streams_.find(c.path); found != streams_.end() && found->second == c.s) {
                        streams_.erase(found);
                    }
                }
                (void)::closesocket(c.s);
                connections.erase(connections.begin() + static_cast<std::ptrdiff_t>(*it));
            }
        }
        for (auto& connection : connections) {
            (void)::closesocket(connection.s);
        }
    }

    void AcceptAll(SOCKET listener, std::size_t instance, Kind kind, std::vector<Connection>& connections) {
        while (true) {
            const SOCKET s = ::accept(listener, nullptr, nullptr);
            if (s == INVALID_SOCKET) {
                return;
            }
            SetNonBlocking(s);
            connections.push_back({s, instance, kind, {}, {}, false});
        }
    }

    // Returns false when the connection should be closed.
    bool Serve(Connection& connection) {
        char chunk[65536];
        const int n = ::recv(connection.s, chunk, sizeof(chunk), 0);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            return ::WSAGetLastError() == WSAEWOULDBLOCK;
        }
        connection.buffer.append(chunk, static_cast<std::size_t>(n));
        switch (connection.kind) {
        case Kind::Control:
            return ServeControl(connection);
        case Kind::DataPending:
            return ServeHello(connection);
        case Kind::DataClient:
            return ServeRecords(connection);
        }
        return false;
    }

    bool ServeControl(Connection& connection) {
        const auto newline = connection.buffer.find('\n');
        if (newline == std::string::npos) {
            return connection.buffer.size() < 65536;
        }
        const Reply reply = HandleRequest(connection.instance, connection.buffer.substr(0, newline));
        if (reply.hang) {
            connection.hung = true;
            return true;
        }
        (void)SendAll(connection.s, Bytes(reply.raw.begin(), reply.raw.end()));
        return false;
    }

    bool ServeHello(Connection& connection) {
        const auto newline = connection.buffer.find('\n');
        if (newline == std::string::npos) {
            return connection.buffer.size() < 65536;
        }
        const std::string line = connection.buffer.substr(0, newline);
        connection.buffer.erase(0, newline + 1);
        std::string reply;
        std::string path;
        {
            std::lock_guard lock{mutex_};
            hellos_.push_back({connection.instance, line});
            changed_.notify_all();
            Instance& instance = instances_[connection.instance];
            if (!instance.hello_refusals.empty()) {
                reply = "{\"ok\":false,\"error\":\"" + instance.hello_refusals.front() + "\"}\n";
                instance.hello_refusals.pop_front();
            } else {
                const nlohmann::json body = nlohmann::json::parse(line, nullptr, false);
                if (!body.is_object() || body.value("hello", "") != "client" || !body.contains("socket") ||
                    !body["socket"].is_string() || body.size() != 2) {
                    violations_.push_back("hello: " + line);
                    reply = "{\"ok\":false,\"error\":\"bad_request\"}\n";
                } else {
                    path = body["socket"];
                    if (path.rfind(directory_ + "\\eln-", 0) != 0 ||
                        path.find('\\', directory_.size() + 1) != std::string::npos) {
                        violations_.push_back("hello socket outside the broker directory: " + line);
                        reply = "{\"ok\":false,\"error\":\"bad_path\"}\n";
                        path.clear();
                    } else if (::GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        reply = "{\"ok\":false,\"error\":\"not_socket\"}\n";
                        path.clear();
                    } else if (streams_.contains(path)) {
                        reply = "{\"ok\":false,\"error\":\"already_connected\"}\n";
                        path.clear();
                    } else {
                        streams_[path] = connection.s;
                        reply = "{\"ok\":true}\n";
                    }
                }
            }
        }
        (void)SendAll(connection.s, Bytes(reply.begin(), reply.end()));
        if (path.empty()) {
            return false;
        }
        connection.kind = Kind::DataClient;
        connection.path = path;
        return connection.buffer.empty() ? true : ServeRecords(connection);
    }

    bool ServeRecords(Connection& connection) {
        while (connection.buffer.size() >= 24) {
            const auto& b = connection.buffer;
            const std::uint32_t length = static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[20])) << 24 |
                                         static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[21])) << 16 |
                                         static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[22])) << 8 |
                                         static_cast<std::uint8_t>(b[23]);
            if (length > 65483) {
                std::lock_guard lock{mutex_};
                violations_.push_back("record length " + std::to_string(length) + " from " + connection.path);
                return false;
            }
            if (connection.buffer.size() < 24 + length) {
                return true;
            }
            Bytes bytes(connection.buffer.begin(), connection.buffer.begin() + 24 + length);
            connection.buffer.erase(0, 24 + length);
            std::lock_guard lock{mutex_};
            Route(connection.instance, connection.path, bytes);
            frames_.push_back({connection.instance, std::move(bytes)});
            ++handled_;
            changed_.notify_all();
        }
        return true;
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
        if (socket.rfind(directory_ + "\\eln-", 0) != 0 ||
            socket.find('\\', directory_.size() + 1) != std::string::npos) {
            return fail("socket outside the broker directory");
        }
        if (op == "bind") {
            if (body.size() != 3 || !body.contains("port") || !body["port"].is_number_unsigned() ||
                body["port"].get<unsigned long long>() > 65535) {
                return fail("bind keys");
            }
            if (!streams_.contains(socket)) {
                return {false, "{\"ok\":false,\"error\":\"not_connected\"}\n"};
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

    // Caller holds mutex_.
    void Route(std::size_t instance_index, const std::string& sender, const Bytes& bytes) {
        const auto frame = OracleDecode(bytes);
        if (!frame) {
            violations_.push_back("guest sent an invalid record from " + sender);
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
        const auto stream = streams_.find(it->second);
        if (stream == streams_.end() || SendAll(stream->second, bytes) != 0) {
            ++unroutable_;
        }
    }

    std::string directory_;
    std::vector<Instance> instances_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> data_reads_paused_{false};
    std::atomic<bool> data_read_pause_observed_{false};

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Request> requests_;
    std::vector<Hello> hellos_;
    std::vector<Frame> frames_;
    std::vector<std::string> violations_;
    std::map<std::pair<Ip, unsigned>, std::string> registry_;
    std::map<std::string, SOCKET> streams_;
    std::vector<SOCKET> close_requests_;
    std::size_t handled_ = 0;
    std::size_t unroutable_ = 0;
    bool routing_ = true;
};

} // namespace Mock
