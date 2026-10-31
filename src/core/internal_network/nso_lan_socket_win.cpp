// SPDX-License-Identifier: GPL-3.0-or-later
//
// Windows implementation of Network::NsoLanSocket. The frame codec lives in nso_lan_socket.cpp
// and is shared with Linux; this file supplies the transport.
//
// Measured on Windows 11 (WINDOWS_PROGRESS.md): AF_UNIX supports SOCK_STREAM only, shutdown()
// does not wake a blocked recv or WSAPoll, closesocket() and a peer close do, MSG_PEEK and
// WSAPoll work on AF_UNIX streams, and SIO_AF_UNIX_GETPEERPID reports the peer process.
//
// Shape: one AF_UNIX stream to the broker's data.sock carries consecutive ELN1 frames (the
// 24-byte header holds each record's length). A reader thread reassembles whole frames into a
// bounded queue (a full queue drops the newest frame, like a full datagram receive buffer) and
// writes one byte per frame on an AF_UNIX pair. The pair's read end is `fd`, the descriptor Eden
// polls: POLLIN means a whole datagram (or EOF) is ready. Recv pops frames and consumes bytes
// one for one; Shutdown(RD) and Close enqueue EOF and signal it, which wakes blocked receivers
// (condition variable) and pollers (the byte) without relying on Winsock shutdown behaviour.
#include "core/internal_network/nso_lan_socket.h"

#ifdef _WIN32

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <random>
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

#include "common/error.h"
#include "common/logging.h"
#include "core/internal_network/nso_lan_platform.h"

namespace Network {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds BindTimeout{2000};
constexpr std::chrono::milliseconds UnbindTimeout{500};
constexpr std::chrono::milliseconds HelloTimeout{2000};
constexpr std::size_t MaxControlResponse = 4096;
constexpr int MaxSocketPathAttempts = 16;
constexpr std::size_t MaxLoggedBrokerError = 120;
constexpr std::size_t MaxQueuedFrames = 512;             ///< like a 4 MiB datagram receive buffer
constexpr std::size_t MaxQueuedBytes = 4 * 1024 * 1024;
constexpr u32 DefaultSendTimeoutMs = 2000;               ///< a stalled broker fails sends, not Eden
constexpr int ReaderPollSliceMs = 50;                    ///< reader re-checks `closing` this often
constexpr int SenderPollSliceMs = 50;
constexpr DWORD SioAfUnixGetPeerPid = 0x58000100;        ///< _WSAIOR(IOC_VENDOR, 256), afunix.h

constexpr u32 SupportedReceiveFlags = FLAG_MSG_PEEK | FLAG_MSG_DONTWAIT;
constexpr u32 SupportedSendFlags = FLAG_MSG_DONTWAIT;

Errno TranslateWsaError(int e) {
    switch (e) {
    case 0:
        return Errno::SUCCESS;
    case WSAEBADF:
    case WSAENOTSOCK:
        return Errno::BADF;
    case WSAEINVAL:
        return Errno::INVAL;
    case WSAEMFILE:
        return Errno::MFILE;
    case WSAESHUTDOWN:
        return Errno::PIPE;
    case WSAECONNABORTED:
        return Errno::CONNABORTED;
    case WSAENOTCONN:
        return Errno::NOTCONN;
    case WSAEWOULDBLOCK:
        return Errno::AGAIN;
    case WSAEACCES:
        return Errno::ACCES;
    case WSAEADDRINUSE:
        return Errno::ADDRINUSE;
    case WSAEADDRNOTAVAIL:
        return Errno::ADDRNOTAVAIL;
    case WSAECONNREFUSED:
        // The broker's socket does not exist or is not listening: the virtual network is down.
        return Errno::NETDOWN;
    case WSAECONNRESET:
        return Errno::CONNRESET;
    case WSAEHOSTUNREACH:
        return Errno::HOSTUNREACH;
    case WSAENETDOWN:
        return Errno::NETDOWN;
    case WSAENETUNREACH:
        return Errno::NETUNREACH;
    case WSAEMSGSIZE:
        return Errno::MSGSIZE;
    case WSAETIMEDOUT:
        return Errno::TIMEDOUT;
    case WSAEINPROGRESS:
        return Errno::INPROGRESS;
    case WSAEISCONN:
        return Errno::ISCONN;
    default:
        return Errno::OTHER;
    }
}

Errno LogAndTranslate(const char* operation, int e) {
    const Errno result = TranslateWsaError(e);
    if (result == Errno::AGAIN) {
        LOG_DEBUG(Network, "{}: {}", operation, Common::NativeErrorToString(e));
    } else {
        LOG_ERROR(Network, "{}: {}", operation, Common::NativeErrorToString(e));
    }
    return result;
}

class ScopedSocket {
public:
    explicit ScopedSocket(SOCKET s_) : s{s_} {}
    ~ScopedSocket() {
        if (s != INVALID_SOCKET) {
            (void)::closesocket(s);
        }
    }
    ScopedSocket(const ScopedSocket&) = delete;
    ScopedSocket& operator=(const ScopedSocket&) = delete;
    [[nodiscard]] SOCKET Get() const {
        return s;
    }
    SOCKET Release() {
        return std::exchange(s, INVALID_SOCKET);
    }
    void Close() {
        if (s != INVALID_SOCKET) {
            (void)::closesocket(std::exchange(s, INVALID_SOCKET));
        }
    }

private:
    SOCKET s;
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

/// Directory part of an absolute native path; nullopt for relative or root-only paths.
std::optional<std::string> ParentDirectory(const std::string& path) {
    const std::filesystem::path p{path};
    if (!p.is_absolute() || !p.has_filename() || p.lexically_normal() != p) {
        return std::nullopt;
    }
    return p.parent_path().string();
}

bool SetNonBlocking(SOCKET s, bool enable) {
    u_long mode = enable ? 1 : 0;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
}

/// Waits until `s` reports `events` or `deadline` passes. Returns true when the socket needs
/// attention (including error conditions, which the next call reports).
bool WaitForSocket(SOCKET s, short events, Clock::time_point deadline) {
    while (true) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) {
            return false;
        }
        const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        WSAPOLLFD entry{s, events, 0};
        const int result = ::WSAPoll(&entry, 1, static_cast<INT>(std::min<long long>(milliseconds, INT_MAX)));
        if (result > 0) {
            return true;
        }
        if (result == 0) {
            return false;
        }
        return true; // let the caller observe the error
    }
}

/// The broker's error code, bounded and with control characters removed.
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

Errno BrokerRefusalErrno(std::string_view code) {
    if (code == "port_in_use") {
        return Errno::ADDRINUSE;
    }
    if (code == "forbidden") {
        return Errno::ACCES;
    }
    if (code == "not_ready") {
        return Errno::NETDOWN;
    }
    if (code == "already_bound") {
        return Errno::INVAL;
    }
    return Errno::OTHER;
}

struct LineReply {
    Errno error = Errno::SUCCESS;
    std::string line; ///< Response without its newline; valid only when error is SUCCESS.
};

/// Sends `request` on a connected, non-blocking stream and reads one response line of at most
/// MaxControlResponse bytes (newline included) before `deadline`.
LineReply ExchangeLine(SOCKET s, const std::string& request, Clock::time_point deadline,
                       const char* what) {
    std::size_t sent = 0;
    while (sent < request.size()) {
        const int written = ::send(s, request.data() + sent, static_cast<int>(request.size() - sent), 0);
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
        } else if (::WSAGetLastError() == WSAEWOULDBLOCK) {
            if (!WaitForSocket(s, POLLWRNORM, deadline)) {
                LOG_ERROR(Network, "LAN {} request timed out", what);
                return {Errno::TIMEDOUT, {}};
            }
        } else {
            return {LogAndTranslate("LAN stream send", ::WSAGetLastError()), {}};
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
            LOG_ERROR(Network, "LAN {} response exceeds {} bytes", what, MaxControlResponse);
            return {Errno::OTHER, {}};
        }
        // Template-argument form: immune to a stray min/max macro if a translation unit includes
        // the Windows headers before NOMINMAX takes effect.
        const int room = static_cast<int>(std::min<std::size_t>(sizeof(chunk), MaxControlResponse - response.size()));
        const int received = ::recv(s, chunk, room, 0);
        if (received > 0) {
            response.append(chunk, static_cast<std::size_t>(received));
        } else if (received == 0) {
            LOG_ERROR(Network, "LAN {} connection closed before a complete response", what);
            return {Errno::OTHER, {}};
        } else if (::WSAGetLastError() == WSAEWOULDBLOCK) {
            if (!WaitForSocket(s, POLLRDNORM, deadline)) {
                LOG_ERROR(Network, "LAN {} response timed out", what);
                return {Errno::TIMEDOUT, {}};
            }
        } else {
            return {LogAndTranslate("LAN stream receive", ::WSAGetLastError()), {}};
        }
    }
}

/// Connects a fresh AF_UNIX stream to `path` and leaves it non-blocking.
std::pair<SOCKET, Errno> ConnectStream(const std::string& path, const char* what) {
    const auto address = MakeUnixAddress(path);
    if (!address) {
        return {INVALID_SOCKET, Errno::INVAL};
    }
    ScopedSocket stream{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (stream.Get() == INVALID_SOCKET) {
        return {INVALID_SOCKET, LogAndTranslate(what, ::WSAGetLastError())};
    }
    // A same-host AF_UNIX connect completes or fails immediately (measured WSAECONNREFUSED).
    if (::connect(stream.Get(), reinterpret_cast<const sockaddr*>(&*address), sizeof(*address)) != 0) {
        return {INVALID_SOCKET, LogAndTranslate(what, ::WSAGetLastError())};
    }
    if (!SetNonBlocking(stream.Get(), true)) {
        return {INVALID_SOCKET, LogAndTranslate(what, ::WSAGetLastError())};
    }
    return {stream.Release(), Errno::SUCCESS};
}

/// One bounded request/response exchange on the broker's control socket.
LineReply ControlRequest(const std::string& control_path, const std::string& request,
                         std::chrono::milliseconds timeout) {
    auto [raw, error] = ConnectStream(control_path, "LAN control connect");
    if (error != Errno::SUCCESS) {
        return {error, {}};
    }
    const ScopedSocket stream{raw};
    return ExchangeLine(stream.Get(), request, Clock::now() + timeout, "control");
}

nlohmann::json ParseReply(const std::string& line) {
    return nlohmann::json::parse(line, nullptr, false);
}

std::string MakeUniqueSocketPath(const std::string& directory) {
    static std::atomic<u64> sequence{0};
    std::random_device device;
    const u64 nonce = static_cast<u64>(device()) << 32 | device();
    char name[96];
    std::snprintf(name, sizeof(name), "eln-%lu-%llu-%016llx.sock",
                  static_cast<unsigned long>(::GetCurrentProcessId()),
                  static_cast<unsigned long long>(sequence.fetch_add(1)),
                  static_cast<unsigned long long>(nonce));
    return (std::filesystem::path{directory} / name).string();
}

std::optional<DWORD> PeerProcessId(SOCKET s) {
    DWORD pid = 0;
    DWORD returned = 0;
    if (::WSAIoctl(s, SioAfUnixGetPeerPid, nullptr, 0, &pid, sizeof(pid), &returned, nullptr, nullptr) != 0) {
        return std::nullopt;
    }
    return pid;
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

} // Anonymous namespace

/// Lifetime: shared between the socket object, every in-flight Recv/Send (which copy the pointer
/// under state_mutex) and the reader thread. Close marks `closing`, wakes everyone, joins the
/// reader and closes the stream once no sender can be inside ::send; the socket object keeps its
/// reference until it is destroyed or re-initialized. The wake-up pair handles are closed here,
/// when the last reference is gone, so a closed handle value can never be reused under a waiter or
/// poller that still holds it.
struct NsoLanSocket::WindowsState {
    SOCKET stream = INVALID_SOCKET;       ///< AF_UNIX stream to the broker's data.sock (non-blocking)
    SOCKET signal_read = INVALID_SOCKET;  ///< read end of the wake-up pair (= `fd` while open)
    SOCKET signal_write = INVALID_SOCKET; ///< our end of the wake-up pair
    std::thread reader;

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::vector<u8>> queue; ///< complete frames (header + payload), oldest first
    std::size_t queued_bytes = 0;
    bool eof = false;        ///< the broker closed the stream or sent an invalid record
    bool eof_signalled = false;
    bool read_shutdown = false; ///< guarded with the queue: cannot lose a receiver wake-up
    bool closing = false;    ///< set by Close under queue_mutex; every waiting path re-checks it
    std::atomic<bool> non_blocking{false};
    u32 receive_timeout_ms = 0;
    std::atomic<u32> send_timeout_ms{DefaultSendTimeoutMs}; ///< 0: no timeout (Close still interrupts)

    std::mutex send_mutex;    ///< serializes records; Close holds it while closing the stream
    bool send_broken = false; ///< a partial frame went out: the stream can no longer be used

    ~WindowsState() {
        if (signal_read != INVALID_SOCKET) {
            (void)::closesocket(signal_read);
        }
        if (signal_write != INVALID_SOCKET) {
            (void)::closesocket(signal_write);
        }
        if (stream != INVALID_SOCKET) {
            (void)::closesocket(stream);
        }
    }

    bool IsClosing() {
        std::lock_guard lock{queue_mutex};
        return closing;
    }

    void Signal() {
        const char byte = 1;
        (void)::send(signal_write, &byte, 1, 0);
    }
};

NsoLanSocket::NsoLanSocket(std::string control_path_, std::string datagram_path_,
                           IPv4Address virtual_address_)
    : control_path{std::move(control_path_)}, datagram_path{std::move(datagram_path_)},
      virtual_address{virtual_address_} {}

NsoLanSocket::~NsoLanSocket() {
    std::lock_guard lock{state_mutex};
    CloseLocked();
    // No call can be in flight here (the descriptor table's snapshots keep the object alive
    // through every call), so withdrawing the handle value is race-free. The handle itself is
    // closed by the shared state's destructor, not by the base class.
    fd = INVALID_SOCKET;
}

bool NsoLanSocket::ClosedLocked() const {
    return !win || win->IsClosing();
}

bool NsoLanSocket::IsOpened() const {
    std::lock_guard lock{state_mutex};
    return !ClosedLocked();
}

// KeepAlive has no UDP meaning, and broadcast destinations remain rejected by the virtual
// address policy regardless of this option (as on Linux). Winsock AF_UNIX rejects these two
// options; accepting them here preserves the guest datagram contract without applying them to
// the implementation-only wake-up stream.
Errno NsoLanSocket::SetKeepAlive(bool) {
    std::lock_guard lock{state_mutex};
    return ClosedLocked() ? Errno::BADF : Errno::SUCCESS;
}

Errno NsoLanSocket::SetBroadcast(bool) {
    std::lock_guard lock{state_mutex};
    return ClosedLocked() ? Errno::BADF : Errno::SUCCESS;
}

Errno NsoLanSocket::SetLinger(bool enable, u32 linger) {
    std::lock_guard lock{state_mutex};
    return ClosedLocked() ? Errno::BADF : Socket::SetLinger(enable, linger);
}

Errno NsoLanSocket::SetReuseAddr(bool enable) {
    std::lock_guard lock{state_mutex};
    return ClosedLocked() ? Errno::BADF : Socket::SetReuseAddr(enable);
}

// These are transport-buffer hints; the reassembled guest receive queue remains bounded at
// 512 frames / 4 MiB. The socket must be logically open even while Close retains its poll handle.
Errno NsoLanSocket::SetSndBuf(u32 value) {
    std::lock_guard lock{state_mutex};
    return ClosedLocked() ? Errno::BADF : Socket::SetSndBuf(value);
}

Errno NsoLanSocket::SetRcvBuf(u32 value) {
    std::lock_guard lock{state_mutex};
    return ClosedLocked() ? Errno::BADF : Socket::SetRcvBuf(value);
}

std::pair<Errno, Errno> NsoLanSocket::GetPendingError() {
    std::lock_guard lock{state_mutex};
    return ClosedLocked() ? std::pair{Errno::SUCCESS, Errno::BADF} : Socket::GetPendingError();
}

Errno NsoLanSocket::Initialize(Domain domain, Type type, Protocol protocol) {
    std::lock_guard lock{state_mutex};
    // Production initializes once, before the descriptor is published. Re-initialization after
    // Close is reachable from tests only and is not protected against a Poll still in flight.
    if (!ClosedLocked()) {
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
    if (!NsoLan::IsPrivateDirectory(*directory)) {
        LOG_ERROR(Network, "LAN broker directory is not a private directory");
        return Errno::INVAL;
    }

    auto state = std::make_unique<WindowsState>();

    // Wake-up pair: listen on a unique node in the private directory, connect to it, accept, and
    // keep the node (the broker validates it as this socket's identity until Close).
    std::string path;
    SOCKET bound = INVALID_SOCKET;
    int bind_error = WSAEADDRINUSE;
    for (int attempt = 0; attempt < MaxSocketPathAttempts && bind_error == WSAEADDRINUSE; ++attempt) {
        path = MakeUniqueSocketPath(*directory);
        const auto address = MakeUnixAddress(path);
        if (!address) {
            LOG_ERROR(Network, "LAN socket path is too long for a Unix socket");
            return Errno::INVAL;
        }
        ScopedSocket candidate{::socket(AF_UNIX, SOCK_STREAM, 0)};
        if (candidate.Get() == INVALID_SOCKET) {
            return LogAndTranslate("LAN wake-up socket", ::WSAGetLastError());
        }
        if (::bind(candidate.Get(), reinterpret_cast<const sockaddr*>(&*address), sizeof(*address)) == 0) {
            bind_error = 0;
            bound = candidate.Release();
        } else {
            bind_error = ::WSAGetLastError();
        }
    }
    if (bind_error != 0) {
        return LogAndTranslate("LAN wake-up socket bind", bind_error);
    }
    ScopedSocket listener{bound};
    const auto remove_node = [&] { (void)::DeleteFileW(std::filesystem::path{path}.c_str()); };
    if (::listen(listener.Get(), 1) != 0) {
        const int e = ::WSAGetLastError();
        remove_node();
        return LogAndTranslate("LAN wake-up socket listen", e);
    }
    auto [write_end, connect_error] = ConnectStream(path, "LAN wake-up socket connect");
    if (connect_error != Errno::SUCCESS) {
        remove_node();
        return connect_error;
    }
    ScopedSocket signal_write{write_end};
    (void)SetNonBlocking(signal_write.Get(), false);
    ScopedSocket read_end{::accept(listener.Get(), nullptr, nullptr)};
    if (read_end.Get() == INVALID_SOCKET) {
        const int e = ::WSAGetLastError();
        remove_node();
        return LogAndTranslate("LAN wake-up socket accept", e);
    }
    // Only our own connection may become the pollable end.
    if (const auto pid = PeerProcessId(read_end.Get()); !pid || *pid != ::GetCurrentProcessId()) {
        remove_node();
        LOG_ERROR(Network, "LAN wake-up socket was connected by another process");
        return Errno::ACCES;
    }
    listener.Close(); // stop listening; the node stays until Close
    (void)SetNonBlocking(read_end.Get(), true);

    // Data stream to the broker, introduced with a hello line naming our node.
    auto [raw_stream, stream_error] = ConnectStream(datagram_path, "LAN datagram stream");
    if (stream_error != Errno::SUCCESS) {
        remove_node();
        return stream_error;
    }
    ScopedSocket stream{raw_stream};
    std::string hello;
    try {
        hello = nlohmann::json{{"hello", "client"}, {"socket", path}}.dump();
    } catch (const nlohmann::json::exception&) {
        remove_node();
        LOG_ERROR(Network, "LAN socket path is not valid UTF-8");
        return Errno::INVAL;
    }
    hello.push_back('\n');
    const LineReply reply = ExchangeLine(stream.Get(), hello, Clock::now() + HelloTimeout, "stream hello");
    if (reply.error != Errno::SUCCESS) {
        remove_node();
        return reply.error;
    }
    const nlohmann::json body = ParseReply(reply.line);
    const auto ok = body.find("ok");
    if (!body.is_object() || ok == body.end() || !ok->is_boolean()) {
        remove_node();
        LOG_ERROR(Network, "LAN broker sent a malformed stream hello response");
        return Errno::OTHER;
    }
    if (!ok->get<bool>()) {
        const std::string code = BrokerErrorCode(body);
        remove_node();
        LOG_ERROR(Network, "LAN broker refused the datagram stream: {}", code);
        return BrokerRefusalErrno(code);
    }
    // The stream stays non-blocking: the reader and senders wait in short WSAPoll slices and
    // re-check `closing`, so Close never has to close a handle another thread is using.
    state->stream = stream.Release();
    state->signal_write = signal_write.Release();
    state->signal_read = read_end.Release();
    fd = state->signal_read;
    local_path = std::move(path);
    win = std::move(state); // a previous (closed) state stays alive for anyone still using it

    const std::shared_ptr<WindowsState> w = win;
    w->reader = std::thread([w] {
        const SOCKET stream_handle = w->stream; // fixed for the thread's lifetime
        // Reads exactly `size` bytes; false on EOF, error or Close (checked every poll slice).
        const auto receive_exact = [&](u8* out, std::size_t size) {
            std::size_t done = 0;
            while (done < size) {
                const int received = ::recv(stream_handle, reinterpret_cast<char*>(out + done),
                                            static_cast<int>(std::min<std::size_t>(size - done, INT_MAX)), 0);
                if (received > 0) {
                    done += static_cast<std::size_t>(received);
                    continue;
                }
                if (received == 0 || ::WSAGetLastError() != WSAEWOULDBLOCK) {
                    return false; // the broker closed the stream or it failed
                }
                if (w->IsClosing()) {
                    return false;
                }
                WSAPOLLFD entry{stream_handle, POLLRDNORM, 0};
                (void)::WSAPoll(&entry, 1, ReaderPollSliceMs);
            }
            return true;
        };
        std::vector<u8> frame;
        frame.reserve(NsoLanWire::HeaderSize + 2048);
        while (true) {
            frame.resize(NsoLanWire::HeaderSize);
            if (!receive_exact(frame.data(), NsoLanWire::HeaderSize)) {
                break;
            }
            // Only the length is needed here; Recv validates the rest and reports INVAL.
            if (frame[0] != 'E' || frame[1] != 'L' || frame[2] != 'N' || frame[3] != '1') {
                LOG_ERROR(Network, "LAN datagram stream lost synchronisation");
                break;
            }
            const u32 payload_size = static_cast<u32>(frame[20]) << 24 | static_cast<u32>(frame[21]) << 16 |
                                     static_cast<u32>(frame[22]) << 8 | static_cast<u32>(frame[23]);
            if (payload_size > NsoLanWire::MaxPayloadSize) {
                LOG_ERROR(Network, "LAN datagram stream carries an oversized record");
                break;
            }
            frame.resize(NsoLanWire::HeaderSize + payload_size);
            if (!receive_exact(frame.data() + NsoLanWire::HeaderSize, payload_size)) {
                break;
            }
            {
                std::lock_guard queue_lock{w->queue_mutex};
                if (w->closing) {
                    break;
                }
                if (w->queue.size() >= MaxQueuedFrames || w->queued_bytes + frame.size() > MaxQueuedBytes) {
                    LOG_DEBUG(Network, "LAN receive queue is full; dropping a datagram");
                    continue;
                }
                w->queued_bytes += frame.size();
                w->queue.push_back(frame);
                w->Signal();
            }
            w->queue_cv.notify_all();
        }
        std::lock_guard queue_lock{w->queue_mutex};
        w->eof = true;
        if (!w->closing && !w->eof_signalled) {
            w->eof_signalled = true;
            w->Signal(); // a poller learns that Recv now returns an error
        }
        w->queue_cv.notify_all();
    });
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
    if (win) {
        {
            std::lock_guard queue_lock{win->queue_mutex};
            win->closing = true;
            if (!win->eof_signalled) {
                win->eof_signalled = true;
                win->Signal(); // a WSAPoll on the wake-up end becomes readable; Recv then reports BADF
            }
        }
        win->queue_cv.notify_all(); // a Recv blocked on the queue re-checks and returns BADF
        if (win->reader.joinable()) {
            win->reader.join(); // exits within one poll slice and never touches a closed handle
        }
        {
            // Senders re-check `closing` between poll slices and never hold send_mutex inside a
            // blocking call, so once we hold it nobody is using the stream handle.
            std::lock_guard send_lock{win->send_mutex};
            if (win->stream != INVALID_SOCKET) {
                (void)::closesocket(std::exchange(win->stream, INVALID_SOCKET));
            }
        }
        // `win` is kept: the state (and the wake-up pair inside it) lives until this object is
        // destroyed or re-initialized and every in-flight call has released its copy, so a poller or
        // receiver still holding the handle value cannot observe a reused handle. `fd` is not
        // written either: Network::Poll reads it without a lock, and the handle it names stays
        // valid; a poller sees the EOF signal byte and the following Recv reports BADF.
    }
    RemoveSocketFileLocked();
    peer.reset();
    shutdown_write = false;
}

void NsoLanSocket::RemoveSocketFileLocked() {
    if (local_path.empty()) {
        return;
    }
    // Only remove a socket node (an AF_UNIX reparse point) at the exact path we created.
    const std::filesystem::path node{local_path};
    const DWORD attributes = ::GetFileAttributesW(node.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        (void)::DeleteFileW(node.c_str());
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

    const LineReply reply = ControlRequest(control_path, request, BindTimeout);
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
    const LineReply reply = ControlRequest(control_path, request, UnbindTimeout);
    if (reply.error != Errno::SUCCESS) {
        LOG_WARNING(Network, "LAN unbind was not acknowledged by the broker");
    }
}

Errno NsoLanSocket::AutoBindLocked() {
    return bound_port != 0 ? Errno::SUCCESS : RegisterLocked(0);
}

Errno NsoLanSocket::Bind(SockAddrIn addr) {
    std::lock_guard lock{state_mutex};
    if (ClosedLocked()) {
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
    if (ClosedLocked()) {
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
    if (ClosedLocked()) {
        return {SockAddrIn{}, Errno::BADF};
    }
    if (!peer) {
        return {SockAddrIn{}, Errno::NOTCONN};
    }
    return {*peer, Errno::SUCCESS};
}

std::pair<SockAddrIn, Errno> NsoLanSocket::GetSockName() {
    std::lock_guard lock{state_mutex};
    if (ClosedLocked()) {
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
    if (ClosedLocked()) {
        return Errno::BADF;
    }
    bool stop_reading = false;
    switch (how) {
    case ShutdownHow::RD:
        stop_reading = true;
        break;
    case ShutdownHow::WR:
        shutdown_write = true;
        break;
    case ShutdownHow::RDWR:
        stop_reading = true;
        shutdown_write = true;
        break;
    default:
        return Errno::INVAL;
    }
    if (stop_reading) {
        // Publish the predicate under the same mutex as the receiver's wait. A notification
        // between its state snapshot and queue lock must still be observable.
        std::lock_guard queue_lock{win->queue_mutex};
        win->read_shutdown = true;
        if (!win->eof_signalled) {
            win->eof_signalled = true;
            win->Signal();
        }
        win->queue_cv.notify_all();
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
        if (ClosedLocked()) {
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
    std::shared_ptr<WindowsState> w;
    {
        std::lock_guard lock{state_mutex};
        if (ClosedLocked()) {
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
        w = win; // shared: Close cannot destroy the state under us
    }
    // One contiguous record per frame, written whole under the send mutex so frames from
    // different guest threads never interleave on the stream.
    thread_local std::vector<u8> record;
    record.resize(NsoLanWire::HeaderSize + message.size());
    const auto encoded = NsoLanWire::EncodeHeader(header);
    std::copy(encoded.begin(), encoded.end(), record.begin());
    std::copy(message.begin(), message.end(), record.begin() + NsoLanWire::HeaderSize);

    const bool dont_wait = (flags & FLAG_MSG_DONTWAIT) != 0 || w->non_blocking.load();
    const u32 timeout_ms = w->send_timeout_ms.load();
    const auto deadline = timeout_ms == 0 ? (Clock::time_point::max)()
                                          : Clock::now() + std::chrono::milliseconds{timeout_ms};
    std::lock_guard send_lock{w->send_mutex};
    if (w->stream == INVALID_SOCKET) {
        return {-1, Errno::BADF}; // closed while we waited for the send lock
    }
    if (w->send_broken) {
        return {-1, Errno::NETDOWN};
    }
    std::size_t sent = 0;
    while (sent < record.size()) {
        const int written = ::send(w->stream, reinterpret_cast<const char*>(record.data() + sent),
                                   static_cast<int>(record.size() - sent), 0);
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        const int e = ::WSAGetLastError();
        if (e == WSAEWOULDBLOCK) {
            if (sent == 0 && dont_wait) {
                return {-1, Errno::AGAIN}; // like a datagram socket with a full buffer
            }
            if (w->IsClosing()) {
                return {-1, Errno::BADF};
            }
            if (Clock::now() >= deadline) {
                if (sent != 0) {
                    w->send_broken = true;
                    LOG_ERROR(Network, "LAN datagram stream send timed out mid-record");
                }
                return {-1, Errno::AGAIN}; // SO_SNDTIMEO expiry reports EAGAIN, as on Linux
            }
            WSAPOLLFD entry{w->stream, POLLWRNORM, 0};
            (void)::WSAPoll(&entry, 1, SenderPollSliceMs);
            continue;
        }
        if (sent != 0) {
            // A partial record corrupts the stream for good; later sends report the network down.
            w->send_broken = true;
            LOG_ERROR(Network, "LAN datagram stream send was interrupted mid-record");
        }
        return {-1, LogAndTranslate("LAN datagram send", e)};
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
    const bool dont_wait = (flags & FLAG_MSG_DONTWAIT) != 0;

    enum class Verdict { Deliver, Reject, Drop };

    while (true) {
        std::shared_ptr<WindowsState> w;
        std::optional<SockAddrIn> connected_peer;
        u16 port;
        {
            std::lock_guard lock{state_mutex};
            if (ClosedLocked()) {
                return {-1, Errno::BADF};
            }
            w = win; // shared: Close cannot destroy the state under us
            connected_peer = peer;
            port = bound_port;
        }

        std::unique_lock queue_lock{w->queue_mutex};
        if (w->closing) {
            return {-1, Errno::BADF};
        }
        if (w->read_shutdown) {
            return {0, Errno::SUCCESS};
        }
        if (w->queue.empty()) {
            if (w->eof) {
                return {-1, Errno::NETDOWN}; // the broker is gone
            }
            if (w->non_blocking.load() || dont_wait) {
                return {-1, Errno::AGAIN};
            }
            const u32 timeout_ms = w->receive_timeout_ms;
            if (timeout_ms == 0) {
                w->queue_cv.wait(queue_lock);
            } else if (w->queue_cv.wait_for(queue_lock, std::chrono::milliseconds{timeout_ms}) ==
                           std::cv_status::timeout &&
                       w->queue.empty()) {
                return {-1, Errno::AGAIN}; // like SO_RCVTIMEO on Linux: EAGAIN
            }
            continue; // re-check shutdown/close state before using the queue
        }

        const std::vector<u8>& datagram = w->queue.front();
        NsoLanWire::Header header{};
        Verdict verdict = Verdict::Reject;
        std::string_view reason;
        const auto status = NsoLanWire::DecodeHeader(datagram, header);
        if (status != NsoLanWire::DecodeStatus::Ok) {
            reason = NsoLanWire::ToString(status);
        } else if (!NsoLanWire::IsAllowedPeerAddress(header.source) || header.source_port == 0) {
            reason = "source is not a valid virtual endpoint";
        } else if (port == 0 || header.destination != virtual_address ||
                   header.destination_port != port) {
            reason = "datagram is not addressed to this socket";
        } else if (connected_peer &&
                   (header.source != connected_peer->ip || header.source_port != connected_peer->portno)) {
            // A connected UDP socket never sees traffic from anyone else.
            verdict = Verdict::Drop;
        } else {
            verdict = Verdict::Deliver;
        }

        const auto consume = [&] {
            w->queued_bytes -= w->queue.front().size();
            w->queue.pop_front();
            // One signal byte per queued frame; never blocks. The handle belongs to the shared
            // state (not the guest-visible fd), so it is open and ours as long as `w` lives.
            char byte;
            (void)::recv(w->signal_read, &byte, 1, 0);
        };

        if (verdict == Verdict::Drop) {
            consume();
            continue;
        }
        if (verdict == Verdict::Reject) {
            // A non-peek receive has already consumed the datagram, so it cannot wedge the queue.
            if (!peek) {
                consume();
            }
            LOG_WARNING(Network, "Rejected LAN datagram: {}", reason);
            return {-1, Errno::INVAL};
        }

        // Native UDP semantics: a short buffer receives the head of the datagram, the rest is
        // discarded (or left queued with MSG_PEEK) and no error is reported.
        const std::size_t copied = std::min<std::size_t>(header.payload_size, message.size());
        if (copied != 0) {
            std::memcpy(message.data(), datagram.data() + NsoLanWire::HeaderSize, copied);
        }
        if (addr != nullptr) {
            *addr = SockAddrIn{.family = Domain::INET,
                               .ip = header.source,
                               .portno = header.source_port};
        }
        if (!peek) {
            consume();
        }
        return {static_cast<s32>(copied), Errno::SUCCESS};
    }
}

Errno NsoLanSocket::SetSndTimeo(u32 value) {
    std::lock_guard lock{state_mutex};
    if (ClosedLocked()) {
        return Errno::BADF;
    }
    win->send_timeout_ms.store(value); // zero means no timeout, as on Linux
    return Errno::SUCCESS;
}

Errno NsoLanSocket::SetRcvTimeo(u32 value) {
    std::lock_guard lock{state_mutex};
    if (ClosedLocked()) {
        return Errno::BADF;
    }
    std::lock_guard queue_lock{win->queue_mutex};
    win->receive_timeout_ms = value;
    return Errno::SUCCESS;
}

Errno NsoLanSocket::SetNonBlock(bool enable) {
    std::lock_guard lock{state_mutex};
    if (ClosedLocked()) {
        return Errno::BADF;
    }
    win->non_blocking.store(enable);
    return Errno::SUCCESS;
}

} // namespace Network

#endif // _WIN32
