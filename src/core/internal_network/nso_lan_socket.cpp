// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/internal_network/nso_lan_socket.h"

#include <algorithm>

namespace Network {

namespace NsoLanWire {

std::string_view ToString(DecodeStatus status) {
    switch (status) {
    case DecodeStatus::Ok:
        return "ok";
    case DecodeStatus::TooShort:
        return "datagram shorter than the header";
    case DecodeStatus::BadMagic:
        return "bad magic";
    case DecodeStatus::BadType:
        return "unsupported datagram type";
    case DecodeStatus::BadReserved:
        return "reserved bytes are not zero";
    case DecodeStatus::PayloadTooLarge:
        return "payload length exceeds the UDP maximum";
    case DecodeStatus::LengthMismatch:
        return "payload length does not match the datagram size";
    }
    return "unknown";
}

std::array<u8, HeaderSize> EncodeHeader(const Header& header) {
    std::array<u8, HeaderSize> bytes{};
    std::copy(Magic.begin(), Magic.end(), bytes.begin());
    bytes[4] = TypeData;
    std::copy(header.source.begin(), header.source.end(), bytes.begin() + 8);
    std::copy(header.destination.begin(), header.destination.end(), bytes.begin() + 12);
    bytes[16] = static_cast<u8>(header.source_port >> 8);
    bytes[17] = static_cast<u8>(header.source_port);
    bytes[18] = static_cast<u8>(header.destination_port >> 8);
    bytes[19] = static_cast<u8>(header.destination_port);
    bytes[20] = static_cast<u8>(header.payload_size >> 24);
    bytes[21] = static_cast<u8>(header.payload_size >> 16);
    bytes[22] = static_cast<u8>(header.payload_size >> 8);
    bytes[23] = static_cast<u8>(header.payload_size);
    return bytes;
}

DecodeStatus DecodeHeader(std::span<const u8> datagram, Header& header) {
    if (datagram.size() < HeaderSize) {
        return DecodeStatus::TooShort;
    }
    if (!std::equal(Magic.begin(), Magic.end(), datagram.begin())) {
        return DecodeStatus::BadMagic;
    }
    if (datagram[4] != TypeData) {
        return DecodeStatus::BadType;
    }
    if (datagram[5] != 0 || datagram[6] != 0 || datagram[7] != 0) {
        return DecodeStatus::BadReserved;
    }
    const u32 payload_size = static_cast<u32>(datagram[20]) << 24 |
                             static_cast<u32>(datagram[21]) << 16 |
                             static_cast<u32>(datagram[22]) << 8 | static_cast<u32>(datagram[23]);
    if (payload_size > MaxPayloadSize) {
        return DecodeStatus::PayloadTooLarge;
    }
    if (datagram.size() - HeaderSize != payload_size) {
        return DecodeStatus::LengthMismatch;
    }
    std::copy_n(datagram.begin() + 8, 4, header.source.begin());
    std::copy_n(datagram.begin() + 12, 4, header.destination.begin());
    header.source_port = static_cast<u16>(datagram[16] << 8 | datagram[17]);
    header.destination_port = static_cast<u16>(datagram[18] << 8 | datagram[19]);
    header.payload_size = payload_size;
    return DecodeStatus::Ok;
}

bool IsPrivateGuestAddress(const IPv4Address& address) {
    if (address[0] != 10 || address[1] != 253) {
        return false;
    }
    const bool network = address[2] == 0 && address[3] == 0;
    const bool broadcast = address[2] == 255 && address[3] == 255;
    return !network && !broadcast;
}

bool IsAllowedPeerAddress(const IPv4Address& address) {
    return IsPrivateGuestAddress(address) || address == IPv4Address{10, 254, 0, 1} ||
           address == IPv4Address{10, 254, 0, 2};
}

} // namespace NsoLanWire

} // namespace Network

#ifdef __linux__

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "common/error.h"
#include "common/logging.h"

namespace Network {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds BindTimeout{2000};
constexpr std::chrono::milliseconds UnbindTimeout{500};
constexpr std::size_t MaxControlResponse = 4096;
constexpr int MaxSocketPathAttempts = 16;
constexpr std::size_t MaxLoggedBrokerError = 120;

constexpr u32 SupportedReceiveFlags = FLAG_MSG_PEEK | FLAG_MSG_DONTWAIT;
constexpr u32 SupportedSendFlags = FLAG_MSG_DONTWAIT;

static_assert(EAGAIN == EWOULDBLOCK);

Errno TranslateErrno(int e) {
    switch (e) {
    case 0:
        return Errno::SUCCESS;
    case EBADF:
        return Errno::BADF;
    case EINVAL:
        return Errno::INVAL;
    case EMFILE:
    case ENFILE:
        return Errno::MFILE;
    case EPIPE:
        return Errno::PIPE;
    case ECONNABORTED:
        return Errno::CONNABORTED;
    case ENOTCONN:
        return Errno::NOTCONN;
    case EAGAIN:
        return Errno::AGAIN;
    case EACCES:
    case EPERM:
        return Errno::ACCES;
    case EADDRINUSE:
        return Errno::ADDRINUSE;
    case EADDRNOTAVAIL:
        return Errno::ADDRNOTAVAIL;
    case ECONNREFUSED:
        return Errno::CONNREFUSED;
    case ECONNRESET:
        return Errno::CONNRESET;
    case EHOSTUNREACH:
        return Errno::HOSTUNREACH;
    case ENETDOWN:
        return Errno::NETDOWN;
    case ENETUNREACH:
        return Errno::NETUNREACH;
    case EMSGSIZE:
        return Errno::MSGSIZE;
    case ETIMEDOUT:
        return Errno::TIMEDOUT;
    case EINPROGRESS:
        return Errno::INPROGRESS;
    case EISCONN:
        return Errno::ISCONN;
    case ENOENT:
        // The broker's control or datagram socket does not exist: the virtual network is down.
        return Errno::NETDOWN;
    default:
        return Errno::OTHER;
    }
}

Errno LogAndTranslate(const char* operation, int e) {
    const Errno result = TranslateErrno(e);
    if (result == Errno::AGAIN) {
        LOG_DEBUG(Network, "{}: {}", operation, Common::NativeErrorToString(e));
    } else {
        LOG_ERROR(Network, "{}: {}", operation, Common::NativeErrorToString(e));
    }
    return result;
}

class ScopedFd {
public:
    explicit ScopedFd(int fd_) : fd{fd_} {}
    ~ScopedFd() {
        if (fd >= 0) {
            (void)::close(fd);
        }
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    [[nodiscard]] int Get() const {
        return fd;
    }

private:
    int fd;
};

std::optional<sockaddr_un> MakeUnixAddress(const std::string& path) {
    sockaddr_un address{};
    if (path.empty() || path.size() >= sizeof(address.sun_path) ||
        path.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    return address;
}

/// Directory part of an absolute path ("/" for a file directly below the root).
std::optional<std::string> ParentDirectory(const std::string& path) {
    if (path.empty() || path.front() != '/') {
        return std::nullopt;
    }
    const auto slash = path.rfind('/');
    if (slash + 1 == path.size()) {
        return std::nullopt;
    }
    return slash == 0 ? std::string{"/"} : path.substr(0, slash);
}

/// The broker directory must be a real directory (not a symlink) owned by the current user and
/// writable by nobody else; otherwise another user could plant or replace sockets inside it.
bool IsPrivateDirectory(const std::string& directory) {
    struct stat info{};
    if (::lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        return false;
    }
    if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return false;
    }
    return info.st_uid == ::geteuid();
}

/// The broker answers a refusal with a short machine code in "error".
Errno BrokerRefusalErrno(std::string_view code) {
    if (code == "port_in_use") {
        return Errno::ADDRINUSE;
    }
    if (code == "forbidden") {
        return Errno::ACCES;
    }
    if (code == "not_ready") {
        return Errno::NETDOWN; // the broker has no virtual identity yet
    }
    if (code == "already_bound") {
        return Errno::INVAL;
    }
    return Errno::OTHER;
}

std::string MakeUniqueSocketPath(const std::string& directory) {
    static std::atomic<u64> sequence{0};
    std::random_device device;
    const u64 nonce = static_cast<u64>(device()) << 32 | device();
    char name[96];
    std::snprintf(name, sizeof(name), "eln-%ld-%llu-%016llx.sock", static_cast<long>(::getpid()),
                  static_cast<unsigned long long>(sequence.fetch_add(1)),
                  static_cast<unsigned long long>(nonce));
    return (directory == "/" ? directory : directory + '/') + name;
}

bool IsBroadcastOrMulticast(const IPv4Address& address) {
    return address == IPv4Address{255, 255, 255, 255} || (address[0] >= 224 && address[0] <= 239) ||
           address == IPv4Address{10, 253, 255, 255};
}

Errno ValidateDestination(const SockAddrIn& destination) {
    if (destination.family != Domain::INET) {
        LOG_ERROR(Network, "LAN socket destination is not an IPv4 address");
        return Errno::INVAL;
    }
    if (IsBroadcastOrMulticast(destination.ip)) {
        LOG_WARNING(Network, "LAN broker does not support broadcast or multicast traffic");
        return Errno::NETUNREACH;
    }
    if (!NsoLanWire::IsAllowedPeerAddress(destination.ip)) {
        LOG_WARNING(Network, "LAN destination {} is outside the virtual network",
                    IPv4AddressToString(destination.ip));
        return Errno::NETUNREACH;
    }
    if (destination.portno == 0) {
        LOG_ERROR(Network, "LAN socket destination port is zero");
        return Errno::INVAL;
    }
    return Errno::SUCCESS;
}

/// Waits until `fd` reports `events` or `deadline` passes. Returns true when the descriptor needs
/// attention (including error conditions, which the next system call reports).
bool WaitForDescriptor(int fd, short events, Clock::time_point deadline) {
    while (true) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) {
            return false;
        }
        const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        pollfd entry{.fd = fd, .events = events, .revents = 0};
        const int result = ::poll(&entry, 1, static_cast<int>(milliseconds));
        if (result > 0) {
            return true;
        }
        if (result == 0) {
            return false;
        }
        if (errno != EINTR) {
            return true;
        }
    }
}

struct ControlReply {
    Errno error = Errno::SUCCESS;
    std::string line; ///< Response without its newline; valid only when error is SUCCESS.
};

/// One bounded request/response exchange on the broker's AF_UNIX stream control socket. The
/// request is a single line; the response is a single line of at most MaxControlResponse bytes
/// (newline included). The whole exchange, connect included, must finish within `timeout`.
ControlReply ControlRequest(const std::string& control_path, const std::string& request,
                            std::chrono::milliseconds timeout) {
    const auto address = MakeUnixAddress(control_path);
    if (!address) {
        return {Errno::INVAL, {}};
    }
    const ScopedFd stream{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (stream.Get() < 0) {
        return {LogAndTranslate("LAN control socket", errno), {}};
    }
    const auto deadline = Clock::now() + timeout;

    while (::connect(stream.Get(), reinterpret_cast<const sockaddr*>(&*address),
                     sizeof(*address)) != 0) {
        if (errno != EINTR) {
            return {LogAndTranslate("LAN control connect", errno), {}};
        }
    }

    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t written =
            ::send(stream.Get(), request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN) {
            if (!WaitForDescriptor(stream.Get(), POLLOUT, deadline)) {
                LOG_ERROR(Network, "LAN control request timed out");
                return {Errno::TIMEDOUT, {}};
            }
        } else {
            return {LogAndTranslate("LAN control send", errno), {}};
        }
    }

    std::string response;
    char chunk[512];
    while (true) {
        if (const auto newline = response.find('\n'); newline != std::string::npos) {
            response.resize(newline);
            return {Errno::SUCCESS, std::move(response)};
        }
        if (response.size() >= MaxControlResponse) {
            LOG_ERROR(Network, "LAN control response exceeds {} bytes", MaxControlResponse);
            return {Errno::OTHER, {}};
        }
        const std::size_t room = std::min(sizeof(chunk), MaxControlResponse - response.size());
        const ssize_t received = ::recv(stream.Get(), chunk, room, 0);
        if (received > 0) {
            response.append(chunk, static_cast<std::size_t>(received));
        } else if (received == 0) {
            LOG_ERROR(Network, "LAN control connection closed before a complete response");
            return {Errno::OTHER, {}};
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN) {
            if (!WaitForDescriptor(stream.Get(), POLLIN, deadline)) {
                LOG_ERROR(Network, "LAN control response timed out");
                return {Errno::TIMEDOUT, {}};
            }
        } else {
            return {LogAndTranslate("LAN control receive", errno), {}};
        }
    }
}

/// The broker's error code, bounded and with control characters removed: it is mapped to an
/// errno and logged, never interpreted any other way.
std::string BrokerErrorCode(const nlohmann::json& body) {
    const auto found = body.find("error");
    if (found == body.end() || !found->is_string()) {
        return "no error text";
    }
    std::string text = found->get<std::string>().substr(0, MaxLoggedBrokerError);
    for (char& c : text) {
        if (c < 0x20 || c > 0x7e) {
            c = '?';
        }
    }
    return text;
}

nlohmann::json ParseReply(const std::string& line) {
    return nlohmann::json::parse(line, nullptr, false);
}

} // Anonymous namespace

NsoLanSocket::NsoLanSocket(std::string control_path_, std::string datagram_path_,
                           IPv4Address virtual_address_)
    : control_path{std::move(control_path_)}, datagram_path{std::move(datagram_path_)},
      virtual_address{virtual_address_} {}

NsoLanSocket::~NsoLanSocket() {
    std::lock_guard lock{state_mutex};
    CloseLocked();
}

Errno NsoLanSocket::Initialize(Domain domain, Type type, Protocol protocol) {
    std::lock_guard lock{state_mutex};
    if (fd != INVALID_SOCKET) {
        LOG_ERROR(Network, "LAN socket is already initialized");
        return Errno::INVAL;
    }
    const bool protocol_ok = protocol == Protocol::UDP || protocol == Protocol::Unspecified ||
                             protocol == Protocol::IP;
    if (domain != Domain::INET || type != Type::DGRAM || !protocol_ok) {
        LOG_ERROR(Network, "LAN socket supports only IPv4 UDP datagram sockets");
        return Errno::INVAL;
    }
    if (!NsoLanWire::IsPrivateGuestAddress(virtual_address)) {
        LOG_ERROR(Network, "LAN virtual address {} is outside 10.253.0.0/16",
                  IPv4AddressToString(virtual_address));
        return Errno::INVAL;
    }

    const auto directory = ParentDirectory(control_path);
    if (!directory || ParentDirectory(datagram_path) != directory ||
        !MakeUnixAddress(control_path) || !MakeUnixAddress(datagram_path)) {
        LOG_ERROR(Network, "LAN control and datagram sockets must be absolute paths that share one "
                           "directory");
        return Errno::INVAL;
    }
    if (!IsPrivateDirectory(*directory)) {
        LOG_ERROR(Network, "LAN broker directory is not a private directory");
        return Errno::INVAL;
    }

    const int host_fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (host_fd < 0) {
        return LogAndTranslate("LAN datagram socket", errno);
    }

    // Never unlink to make room: a name collision just selects another unique name.
    std::string path;
    int bind_errno = EADDRINUSE;
    for (int attempt = 0; attempt < MaxSocketPathAttempts && bind_errno == EADDRINUSE; ++attempt) {
        path = MakeUniqueSocketPath(*directory);
        const auto address = MakeUnixAddress(path);
        if (!address) {
            (void)::close(host_fd);
            LOG_ERROR(Network, "LAN socket path is too long for a Unix socket");
            return Errno::INVAL;
        }
        bind_errno = ::bind(host_fd, reinterpret_cast<const sockaddr*>(&*address),
                            sizeof(*address)) == 0
                         ? 0
                         : errno;
    }
    if (bind_errno != 0) {
        (void)::close(host_fd);
        return LogAndTranslate("LAN datagram socket bind", bind_errno);
    }

    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISSOCK(info.st_mode)) {
        // We cannot identify the node we just created, so we must not remove it later.
        const int e = errno;
        (void)::close(host_fd);
        return LogAndTranslate("LAN datagram socket identity", e);
    }

    fd = host_fd;
    local_path = std::move(path);
    local_device = static_cast<u64>(info.st_dev);
    local_inode = static_cast<u64>(info.st_ino);
    return Errno::SUCCESS;
}

Errno NsoLanSocket::Close() {
    std::lock_guard lock{state_mutex};
    CloseLocked();
    return Errno::SUCCESS;
}

void NsoLanSocket::CloseLocked() {
    // Unbind first so the broker stops forwarding to a socket that is about to disappear.
    UnregisterLocked();
    if (fd != INVALID_SOCKET) {
        // close alone need not wake a thread already blocked in recvmsg on Linux.
        (void)::shutdown(fd, SHUT_RDWR);
        if (::close(fd) != 0) {
            LOG_WARNING(Network, "closing the LAN datagram socket failed");
        }
        fd = INVALID_SOCKET;
    }
    RemoveSocketFileLocked();
    peer.reset();
    shutdown_read = false;
    shutdown_write = false;
}

void NsoLanSocket::RemoveSocketFileLocked() {
    if (local_path.empty()) {
        return;
    }
    // Only remove the exact node this socket created.
    struct stat info{};
    if (::lstat(local_path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode) &&
        static_cast<u64>(info.st_dev) == local_device &&
        static_cast<u64>(info.st_ino) == local_inode) {
        (void)::unlink(local_path.c_str());
    } else {
        LOG_WARNING(Network, "LAN socket path no longer refers to this socket; leaving it alone");
    }
    local_path.clear();
}

Errno NsoLanSocket::RegisterLocked(u16 requested_port) {
    std::string request;
    try {
        request = nlohmann::json{{"op", "bind"}, {"socket", local_path}, {"port", requested_port}}
                      .dump();
    } catch (const nlohmann::json::exception&) {
        LOG_ERROR(Network, "LAN socket path is not valid UTF-8");
        return Errno::INVAL;
    }
    request.push_back('\n');

    const ControlReply reply = ControlRequest(control_path, request, BindTimeout);
    if (reply.error != Errno::SUCCESS) {
        return reply.error;
    }
    const nlohmann::json body = ParseReply(reply.line);
    const auto ok = body.find("ok");
    if (!body.is_object() || ok == body.end() || !ok->is_boolean()) {
        LOG_ERROR(Network, "LAN broker sent a malformed bind response");
        return Errno::OTHER;
    }
    if (!ok->get<bool>()) {
        const std::string code = BrokerErrorCode(body);
        LOG_ERROR(Network, "LAN broker refused bind: {}", code);
        return BrokerRefusalErrno(code);
    }
    const auto port = body.find("port");
    if (port == body.end() || !port->is_number_unsigned()) {
        LOG_ERROR(Network, "LAN broker bind response has no integer port");
        return Errno::OTHER;
    }
    const u64 actual = port->get<u64>();
    if (actual == 0 || actual > 0xffff) {
        LOG_ERROR(Network, "LAN broker bind response port {} is not a valid guest port", actual);
        return Errno::OTHER;
    }
    // From here the broker holds a registration for this socket.
    bound_port = static_cast<u16>(actual);
    if (requested_port != 0 && bound_port != requested_port) {
        LOG_ERROR(Network, "LAN broker bound port {} instead of requested {}", bound_port,
                  requested_port);
        UnregisterLocked();
        return Errno::OTHER;
    }
    return Errno::SUCCESS;
}

void NsoLanSocket::UnregisterLocked() {
    if (bound_port == 0) {
        return;
    }
    bound_port = 0;
    std::string request;
    try {
        request = nlohmann::json{{"op", "unbind"}, {"socket", local_path}}.dump();
    } catch (const nlohmann::json::exception&) {
        return;
    }
    request.push_back('\n');
    // Best effort: the socket is going away regardless of what the broker answers.
    const ControlReply reply = ControlRequest(control_path, request, UnbindTimeout);
    if (reply.error != Errno::SUCCESS) {
        LOG_WARNING(Network, "LAN unbind was not acknowledged by the broker");
    }
}

Errno NsoLanSocket::AutoBindLocked() {
    return bound_port != 0 ? Errno::SUCCESS : RegisterLocked(0);
}

Errno NsoLanSocket::Bind(SockAddrIn addr) {
    std::lock_guard lock{state_mutex};
    if (fd == INVALID_SOCKET) {
        return Errno::BADF;
    }
    if (addr.family != Domain::INET) {
        LOG_ERROR(Network, "LAN socket can only bind IPv4 addresses");
        return Errno::INVAL;
    }
    if (addr.ip != IPv4Address{} && addr.ip != virtual_address) {
        LOG_ERROR(Network, "LAN socket can only bind 0.0.0.0 or its own virtual address");
        return Errno::ADDRNOTAVAIL;
    }
    if (bound_port != 0) {
        LOG_ERROR(Network, "LAN socket is already bound");
        return Errno::INVAL;
    }
    return RegisterLocked(addr.portno);
}

Errno NsoLanSocket::Connect(SockAddrIn addr_in) {
    std::lock_guard lock{state_mutex};
    if (fd == INVALID_SOCKET) {
        return Errno::BADF;
    }
    if (const Errno error = ValidateDestination(addr_in); error != Errno::SUCCESS) {
        return error;
    }
    if (const Errno error = AutoBindLocked(); error != Errno::SUCCESS) {
        return error;
    }
    peer = addr_in;
    return Errno::SUCCESS;
}

std::pair<SockAddrIn, Errno> NsoLanSocket::GetPeerName() {
    std::lock_guard lock{state_mutex};
    if (fd == INVALID_SOCKET) {
        return {SockAddrIn{}, Errno::BADF};
    }
    if (!peer) {
        return {SockAddrIn{}, Errno::NOTCONN};
    }
    return {*peer, Errno::SUCCESS};
}

std::pair<SockAddrIn, Errno> NsoLanSocket::GetSockName() {
    std::lock_guard lock{state_mutex};
    if (fd == INVALID_SOCKET) {
        return {SockAddrIn{}, Errno::BADF};
    }
    return {SockAddrIn{.family = Domain::INET,
                       .ip = bound_port != 0 ? virtual_address : IPv4Address{},
                       .portno = bound_port},
            Errno::SUCCESS};
}

std::pair<SocketBase::AcceptResult, Errno> NsoLanSocket::Accept() {
    LOG_WARNING(Network, "accept is not supported on a LAN datagram socket");
    return {AcceptResult{}, Errno::OTHER};
}

Errno NsoLanSocket::Listen(s32) {
    LOG_WARNING(Network, "listen is not supported on a LAN datagram socket");
    return Errno::OTHER;
}

Errno NsoLanSocket::Shutdown(ShutdownHow how) {
    std::lock_guard lock{state_mutex};
    if (fd == INVALID_SOCKET) {
        return Errno::BADF;
    }
    int native_how;
    switch (how) {
    case ShutdownHow::RD:
        shutdown_read = true;
        native_how = SHUT_RD;
        break;
    case ShutdownHow::WR:
        shutdown_write = true;
        native_how = SHUT_WR;
        break;
    case ShutdownHow::RDWR:
        shutdown_read = true;
        shutdown_write = true;
        native_how = SHUT_RDWR;
        break;
    default:
        return Errno::INVAL;
    }
    // Native UDP applies shutdown even when reporting ENOTCONN. The transport descriptor is
    // unconnected AF_UNIX; guest connection state is held separately in peer.
    if (::shutdown(fd, native_how) != 0 && errno != ENOTCONN) {
        return LogAndTranslate("LAN datagram shutdown", errno);
    }
    return peer ? Errno::SUCCESS : Errno::NOTCONN;
}

std::pair<s32, Errno> NsoLanSocket::Recv(int flags, std::span<u8> message) {
    return ReceiveDatagram(static_cast<u32>(flags), message, nullptr);
}

std::pair<s32, Errno> NsoLanSocket::RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) {
    return ReceiveDatagram(static_cast<u32>(flags), message, addr);
}

std::pair<s32, Errno> NsoLanSocket::Send(std::span<const u8> message, int flags) {
    SockAddrIn destination{};
    {
        std::lock_guard lock{state_mutex};
        if (fd == INVALID_SOCKET) {
            return {-1, Errno::BADF};
        }
        if (!peer) {
            return {-1, Errno::NOTCONN};
        }
        destination = *peer;
    }
    return SendDatagram(destination, message, static_cast<u32>(flags));
}

std::pair<s32, Errno> NsoLanSocket::SendTo(u32 flags, std::span<const u8> message,
                                           const SockAddrIn* addr) {
    if (addr == nullptr) {
        return Send(message, static_cast<int>(flags));
    }
    return SendDatagram(*addr, message, flags);
}

std::pair<s32, Errno> NsoLanSocket::SendDatagram(const SockAddrIn& destination,
                                                 std::span<const u8> message, u32 flags) {
    NsoLanWire::Header header{};
    int host_fd;
    {
        std::lock_guard lock{state_mutex};
        if (fd == INVALID_SOCKET) {
            return {-1, Errno::BADF};
        }
        if (shutdown_write) {
            return {-1, Errno::PIPE};
        }
        if ((flags & ~SupportedSendFlags) != 0) {
            LOG_ERROR(Network, "Unsupported LAN send flags {:#x}", flags);
            return {-1, Errno::INVAL};
        }
        if (message.size() > NsoLanWire::MaxPayloadSize) {
            return {-1, Errno::MSGSIZE};
        }
        if (const Errno error = ValidateDestination(destination); error != Errno::SUCCESS) {
            return {-1, error};
        }
        if (const Errno error = AutoBindLocked(); error != Errno::SUCCESS) {
            return {-1, error};
        }
        header = NsoLanWire::Header{.source = virtual_address,
                                    .destination = destination.ip,
                                    .source_port = bound_port,
                                    .destination_port = destination.portno,
                                    .payload_size = static_cast<u32>(message.size())};
        host_fd = fd;
    }

    auto broker = MakeUnixAddress(datagram_path);
    if (!broker) {
        return {-1, Errno::INVAL};
    }
    auto encoded = NsoLanWire::EncodeHeader(header);
    iovec parts[2] = {
        {.iov_base = encoded.data(), .iov_len = encoded.size()},
        // sendmsg does not modify the payload; iovec just lacks a const form.
        {.iov_base = const_cast<u8*>(message.data()), .iov_len = message.size()},
    };
    msghdr request{};
    request.msg_name = &*broker;
    request.msg_namelen = sizeof(*broker);
    request.msg_iov = parts;
    request.msg_iovlen = 2;

    int host_flags = MSG_NOSIGNAL;
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        host_flags |= MSG_DONTWAIT;
    }
    ssize_t result;
    do {
        result = ::sendmsg(host_fd, &request, host_flags);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        return {-1, LogAndTranslate("LAN datagram send", errno)};
    }
    if (static_cast<std::size_t>(result) != encoded.size() + message.size()) {
        LOG_ERROR(Network, "LAN datagram send was truncated");
        return {-1, Errno::OTHER};
    }
    return {static_cast<s32>(message.size()), Errno::SUCCESS};
}

std::pair<s32, Errno> NsoLanSocket::ReceiveDatagram(u32 flags, std::span<u8> message,
                                                    SockAddrIn* addr) {
    if ((flags & ~SupportedReceiveFlags) != 0) {
        LOG_ERROR(Network, "Unsupported LAN receive flags {:#x}", flags);
        return {-1, Errno::INVAL};
    }
    const bool peek = (flags & FLAG_MSG_PEEK) != 0;
    int host_flags = 0;
    if (peek) {
        host_flags |= MSG_PEEK;
    }
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        host_flags |= MSG_DONTWAIT;
    }

    enum class Verdict { Deliver, Reject, Drop };

    // One byte more than the largest frame lets the kernel tell us a datagram was oversized.
    thread_local std::vector<u8> buffer(NsoLanWire::HeaderSize + NsoLanWire::MaxPayloadSize + 1);

    while (true) {
        int host_fd;
        {
            std::lock_guard lock{state_mutex};
            if (fd == INVALID_SOCKET) {
                return {-1, Errno::BADF};
            }
            if (shutdown_read) {
                return {0, Errno::SUCCESS};
            }
            host_fd = fd;
        }

        iovec part{.iov_base = buffer.data(), .iov_len = buffer.size()};
        msghdr incoming{};
        incoming.msg_iov = &part;
        incoming.msg_iovlen = 1;
        const ssize_t received = ::recvmsg(host_fd, &incoming, host_flags);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {-1, LogAndTranslate("LAN datagram receive", errno)};
        }

        Verdict verdict = Verdict::Reject;
        std::string_view reason;
        NsoLanWire::Header header{};
        {
            std::lock_guard lock{state_mutex};
            // A zero-length transport read after shutdown is EOF, not an ELN1 datagram.
            // Real zero-length guest payloads still contain the complete 24-byte header.
            if (fd == INVALID_SOCKET || (received == 0 && shutdown_read)) {
                return {0, Errno::SUCCESS};
            }
            const std::span<const u8> datagram{buffer.data(), static_cast<std::size_t>(received)};
            const auto status = (incoming.msg_flags & MSG_TRUNC) != 0
                                    ? NsoLanWire::DecodeStatus::PayloadTooLarge
                                    : NsoLanWire::DecodeHeader(datagram, header);
            if (status != NsoLanWire::DecodeStatus::Ok) {
                reason = NsoLanWire::ToString(status);
            } else if (!NsoLanWire::IsAllowedPeerAddress(header.source) ||
                       header.source_port == 0) {
                reason = "source is not a valid virtual endpoint";
            } else if (bound_port == 0 || header.destination != virtual_address ||
                       header.destination_port != bound_port) {
                reason = "datagram is not addressed to this socket";
            } else if (peer && (header.source != peer->ip || header.source_port != peer->portno)) {
                // A connected UDP socket never sees traffic from anyone else.
                verdict = Verdict::Drop;
            } else {
                verdict = Verdict::Deliver;
            }
        }

        if (verdict == Verdict::Drop) {
            if (peek) {
                // The filtered datagram would never have been queued natively; discard it so
                // the peek reports on the next datagram that is actually deliverable.
                iovec scratch{.iov_base = buffer.data(), .iov_len = buffer.size()};
                msghdr discard{};
                discard.msg_iov = &scratch;
                discard.msg_iovlen = 1;
                (void)::recvmsg(host_fd, &discard, MSG_DONTWAIT);
            }
            continue;
        }
        if (verdict == Verdict::Reject) {
            // A non-peek receive has already consumed the datagram, so it cannot wedge the queue.
            LOG_WARNING(Network, "Rejected LAN datagram: {}", reason);
            return {-1, Errno::INVAL};
        }

        // Native UDP semantics: a short buffer receives the head of the datagram, the rest is
        // discarded (or left queued with MSG_PEEK) and no error is reported.
        const std::size_t copied = std::min<std::size_t>(header.payload_size, message.size());
        if (copied != 0) {
            std::memcpy(message.data(), buffer.data() + NsoLanWire::HeaderSize, copied);
        }
        if (addr != nullptr) {
            *addr = SockAddrIn{.family = Domain::INET,
                               .ip = header.source,
                               .portno = header.source_port};
        }
        return {static_cast<s32>(copied), Errno::SUCCESS};
    }
}

Errno NsoLanSocket::SetSndTimeo(u32 value) {
    return SetTimeout(SO_SNDTIMEO, value);
}

Errno NsoLanSocket::SetRcvTimeo(u32 value) {
    return SetTimeout(SO_RCVTIMEO, value);
}

Errno NsoLanSocket::SetTimeout(int option, u32 milliseconds) {
    std::lock_guard lock{state_mutex};
    if (fd == INVALID_SOCKET) {
        return Errno::BADF;
    }
    // Linux wants a timeval here; the generic Socket helper would pass a bare integer, which
    // the kernel rejects.
    const timeval timeout{.tv_sec = static_cast<time_t>(milliseconds / 1000),
                          .tv_usec = static_cast<suseconds_t>(milliseconds % 1000 * 1000)};
    if (::setsockopt(fd, SOL_SOCKET, option, &timeout, sizeof(timeout)) == 0) {
        return Errno::SUCCESS;
    }
    return LogAndTranslate("LAN socket timeout", errno);
}

} // namespace Network

#endif // __linux__
