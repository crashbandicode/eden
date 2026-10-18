// SPDX-License-Identifier: GPL-3.0-or-later
// Regression probe for Network::NsoLanSocket. It runs the production class and the unmodified
// base Socket/Poll implementation against a mock broker that speaks the real wire protocol over
// real Unix sockets (see mock_broker.h). Expected bytes are literals written from the wire
// description, not produced by production code.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/internal_network/network.h"
#include "core/internal_network/nso_lan_socket.h"
#include "core/internal_network/sockets.h"
#include "mock_broker.h"
#include "test_support.h"

using namespace Network;
using namespace std::chrono_literals;
using namespace std::string_literals;
using Mock::Bytes;
using Mock::Ip;

namespace {

// ---------------------------------------------------------------------------------------------
// Minimal harness
// ---------------------------------------------------------------------------------------------

struct TestFailure {
    std::string message;
};

[[noreturn]] void Fail(const char* file, int line, const std::string& what) {
    throw TestFailure{std::string{file} + ":" + std::to_string(line) + ": " + what};
}

std::string Show(Errno e) {
    switch (e) {
    case Errno::SUCCESS: return "SUCCESS";
    case Errno::BADF: return "BADF";
    case Errno::INVAL: return "INVAL";
    case Errno::MFILE: return "MFILE";
    case Errno::PIPE: return "PIPE";
    case Errno::NOTCONN: return "NOTCONN";
    case Errno::AGAIN: return "AGAIN";
    case Errno::CONNREFUSED: return "CONNREFUSED";
    case Errno::CONNRESET: return "CONNRESET";
    case Errno::CONNABORTED: return "CONNABORTED";
    case Errno::HOSTUNREACH: return "HOSTUNREACH";
    case Errno::NETDOWN: return "NETDOWN";
    case Errno::NETUNREACH: return "NETUNREACH";
    case Errno::TIMEDOUT: return "TIMEDOUT";
    case Errno::MSGSIZE: return "MSGSIZE";
    case Errno::INPROGRESS: return "INPROGRESS";
    case Errno::ISCONN: return "ISCONN";
    case Errno::OTHER: return "OTHER";
    case Errno::ACCES: return "ACCES";
    case Errno::ADDRINUSE: return "ADDRINUSE";
    case Errno::ADDRNOTAVAIL: return "ADDRNOTAVAIL";
    }
    return "?";
}
std::string Show(const Bytes& bytes) {
    std::ostringstream out;
    out << '[' << bytes.size() << "]:";
    for (std::size_t i = 0; i < bytes.size() && i < 48; ++i) {
        char hex[4];
        std::snprintf(hex, sizeof(hex), "%02x", bytes[i]);
        out << (i ? " " : "") << hex;
    }
    return out.str();
}
std::string Show(const Ip& ip) {
    return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
           std::to_string(ip[3]);
}
std::string Show(const std::string& s) {
    return '"' + s + '"';
}
std::string Show(bool b) {
    return b ? "true" : "false";
}
template <typename T>
    requires std::is_arithmetic_v<T>
std::string Show(T value) {
    return std::to_string(value);
}
std::string Show(const std::pair<s32, Errno>& result) {
    return "{" + std::to_string(result.first) + ", " + Show(result.second) + "}";
}

template <typename A, typename B>
void CheckEq(const char* file, int line, const char* text, const A& actual, const B& expected) {
    if (!(actual == expected)) {
        Fail(file, line, std::string{text} + " is " + Show(actual) + ", expected " + Show(expected));
    }
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            Fail(__FILE__, __LINE__, #condition);                                                  \
        }                                                                                          \
    } while (0)
#define CHECK_EQ(actual, expected) CheckEq(__FILE__, __LINE__, #actual, (actual), (expected))
#define CHECK_ADDR(actual_addr, expected_ip, expected_port)                                        \
    do {                                                                                           \
        const SockAddrIn checked_ = (actual_addr);                                                 \
        CheckEq(__FILE__, __LINE__, #actual_addr ".family", static_cast<int>(checked_.family),     \
                static_cast<int>(Domain::INET));                                                   \
        CheckEq(__FILE__, __LINE__, #actual_addr ".ip", checked_.ip, Ip(expected_ip));             \
        CheckEq(__FILE__, __LINE__, #actual_addr ".port", checked_.portno,                         \
                static_cast<u16>(expected_port));                                                  \
    } while (0)

// ---------------------------------------------------------------------------------------------
// Fixtures and helpers
// ---------------------------------------------------------------------------------------------

constexpr Ip A{10, 253, 0, 2};
constexpr Ip B{10, 253, 0, 3};
constexpr Ip Any{0, 0, 0, 0};
constexpr Ip ServiceOne{10, 254, 0, 1};
constexpr Ip ServiceTwo{10, 254, 0, 2};

Bytes Text(const std::string& text) {
    return Bytes(text.begin(), text.end());
}
SockAddrIn Addr(Ip ip, u16 port) {
    return SockAddrIn{.family = Domain::INET, .ip = ip, .portno = port};
}
Bytes Pattern(std::size_t size) {
    Bytes out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    return out;
}

double ThreadCpuMs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) / 1e6;
}

class Stopwatch {
public:
    double Ms() const {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
            .count();
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

std::size_t OpenFdCount() {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        ++count;
    }
    return count;
}

// One private work directory plus a mock broker serving the given virtual addresses.
class Env {
public:
    explicit Env(std::initializer_list<Ip> addresses, std::string pattern = "/tmp/eln-XXXXXX") {
        if (::mkdtemp(pattern.data()) == nullptr) {
            Fail(__FILE__, __LINE__, "mkdtemp failed");
        }
        dir = pattern;
        broker = std::make_unique<Mock::Broker>(dir);
        for (const Ip& address : addresses) {
            broker->AddInstance(address);
        }
        CHECK(broker->Start());
    }
    ~Env() {
        broker->Stop();
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }

    std::unique_ptr<NsoLanSocket> Raw(std::size_t instance) {
        return std::make_unique<NsoLanSocket>(broker->ControlPath(instance),
                                              broker->DatagramPath(instance),
                                              broker->Address(instance));
    }
    std::unique_ptr<NsoLanSocket> Open(std::size_t instance) {
        auto socket = Raw(instance);
        CHECK_EQ(socket->Initialize(Domain::INET, Type::DGRAM, Protocol::UDP), Errno::SUCCESS);
        return socket;
    }
    // Names of the guest sockets currently present in the broker directory.
    std::vector<std::string> SocketFiles() const {
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            const auto name = entry.path().filename().string();
            if (name.rfind("eln-", 0) == 0) {
                names.push_back(name);
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }
    // Must be called at the end of a test: the mock saw only well-formed traffic.
    void ExpectCleanBroker() const {
        const auto violations = broker->Violations();
        if (!violations.empty()) {
            Fail(__FILE__, __LINE__, "mock broker protocol violation: " + violations.front());
        }
    }

    std::string dir;
    std::unique_ptr<Mock::Broker> broker;
};

struct Received {
    s32 size;
    Errno error;
    Bytes data;
    SockAddrIn from;
    bool untouched; // Output buffer beyond `size` and the address (on failure) were not written.
};

Received Receive(SocketBase& socket, std::size_t capacity, int flags = 0) {
    constexpr std::uint8_t Sentinel = 0xEE;
    Bytes buffer(capacity, Sentinel);
    SockAddrIn from{.family = Domain::Unspecified, .ip = {9, 9, 9, 9}, .portno = 9};
    const auto [size, error] = socket.RecvFrom(flags, buffer, &from);
    Received result{size, error, {}, from, true};
    const std::size_t written = size > 0 ? static_cast<std::size_t>(size) : 0;
    result.data.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(written));
    for (std::size_t i = written; i < buffer.size(); ++i) {
        result.untouched = result.untouched && buffer[i] == Sentinel;
    }
    if (error != Errno::SUCCESS) {
        result.untouched = result.untouched && from.family == Domain::Unspecified &&
                           from.ip == Ip{9, 9, 9, 9} && from.portno == 9;
    }
    return result;
}

std::pair<s32, Errno> Send(SocketBase& socket, const Bytes& payload, SockAddrIn to, u32 flags = 0) {
    return socket.SendTo(flags, payload, &to);
}

bool Readable(SocketBase& socket, s32 timeout_ms) {
    std::vector<PollFD> fds{PollFD{&socket, PollEvents::In, PollEvents{}}};
    const auto [count, error] = Poll(fds, timeout_ms);
    CHECK_EQ(error, Errno::SUCCESS);
    return count > 0 && (static_cast<u16>(fds[0].revents) & static_cast<u16>(PollEvents::In)) != 0;
}

std::string SocketPathOf(const Mock::Request& request) {
    return nlohmann::json::parse(request.line).at("socket").get<std::string>();
}

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

// Literal wire bytes for A:5000 -> B:5000 carrying "hello".
const Bytes HelloFrame{0x45, 0x4C, 0x4E, 0x31, 0x01, 0x00, 0x00, 0x00, // "ELN1", type 1, reserved
                       0x0A, 0xFD, 0x00, 0x02,                         // source 10.253.0.2
                       0x0A, 0xFD, 0x00, 0x03,                         // destination 10.253.0.3
                       0x13, 0x88, 0x13, 0x88,                         // ports 5000 -> 5000
                       0x00, 0x00, 0x00, 0x05,                         // payload length 5
                       'h',  'e',  'l',  'l',  'o'};

void WireCodecMatchesLiteralBytes() {
    using namespace NsoLanWire;
    const auto encoded = EncodeHeader(Header{A, B, 5000, 5000, 5});
    CHECK_EQ(Bytes(encoded.begin(), encoded.end()), Bytes(HelloFrame.begin(), HelloFrame.begin() + 24));

    Header header{};
    CHECK(DecodeHeader(HelloFrame, header) == DecodeStatus::Ok);
    CHECK_EQ(header.source, A);
    CHECK_EQ(header.destination, B);
    CHECK_EQ(header.source_port, static_cast<u16>(5000));
    CHECK_EQ(header.destination_port, static_cast<u16>(5000));
    CHECK_EQ(header.payload_size, 5u);

    // Each corruption is made on the literal bytes, so it exercises exactly one rule.
    const auto mutated = [&](std::size_t index, std::uint8_t value) {
        Bytes copy = HelloFrame;
        copy[index] = value;
        Header scratch{};
        return DecodeHeader(copy, scratch);
    };
    CHECK(mutated(0, 'X') == DecodeStatus::BadMagic);
    CHECK(mutated(3, '2') == DecodeStatus::BadMagic);
    CHECK(mutated(4, 0) == DecodeStatus::BadType);
    CHECK(mutated(4, 2) == DecodeStatus::BadType);
    CHECK(mutated(5, 1) == DecodeStatus::BadReserved);
    CHECK(mutated(6, 1) == DecodeStatus::BadReserved);
    CHECK(mutated(7, 0x80) == DecodeStatus::BadReserved);
    CHECK(mutated(23, 4) == DecodeStatus::LengthMismatch);
    CHECK(mutated(23, 6) == DecodeStatus::LengthMismatch);
    CHECK(mutated(20, 1) == DecodeStatus::PayloadTooLarge);
    Header scratch{};
    CHECK(DecodeHeader(std::span<const u8>{HelloFrame.data(), 23}, scratch) == DecodeStatus::TooShort);
    CHECK(DecodeHeader(std::span<const u8>{}, scratch) == DecodeStatus::TooShort);
    Bytes trailing = HelloFrame;
    trailing.push_back(0);
    CHECK(DecodeHeader(trailing, scratch) == DecodeStatus::LengthMismatch);
    // The envelope plus payload must fit one UDP datagram (24 + 65483 = 65507). A length of
    // exactly 65483 is valid; 65484 is not, even when the datagram really is that long.
    CHECK_EQ(NsoLanWire::HeaderSize + NsoLanWire::MaxPayloadSize, std::size_t{65507});
    const Bytes largest = Mock::OracleEncode(A, B, 1, 2, Bytes(65483));
    CHECK_EQ(largest.size(), std::size_t{65507});
    CHECK(DecodeHeader(largest, scratch) == DecodeStatus::Ok);
    Bytes oversized = largest;
    oversized.push_back(0);
    oversized[23] = 0xCC; // 65484 = 0x0000FFCC (65483 is ...FFCB)
    CHECK(DecodeHeader(oversized, scratch) == DecodeStatus::PayloadTooLarge);

    // Address policy, written out as a table.
    CHECK(IsPrivateGuestAddress({10, 253, 0, 1}));
    CHECK(IsPrivateGuestAddress({10, 253, 255, 254}));
    CHECK(IsPrivateGuestAddress({10, 253, 17, 0}));
    CHECK(!IsPrivateGuestAddress({10, 253, 0, 0}));
    CHECK(!IsPrivateGuestAddress({10, 253, 255, 255}));
    CHECK(!IsPrivateGuestAddress({10, 252, 0, 1}));
    CHECK(!IsPrivateGuestAddress({10, 254, 0, 1}));
    CHECK(!IsPrivateGuestAddress({11, 253, 0, 1}));
    CHECK(!IsPrivateGuestAddress({127, 0, 0, 1}));
    CHECK(!IsPrivateGuestAddress({0, 0, 0, 0}));
    CHECK(IsAllowedPeerAddress({10, 254, 0, 1}));
    CHECK(IsAllowedPeerAddress({10, 254, 0, 2}));
    CHECK(!IsAllowedPeerAddress({10, 254, 0, 0}));
    CHECK(!IsAllowedPeerAddress({10, 254, 0, 3}));
    CHECK(!IsAllowedPeerAddress({10, 255, 0, 1}));
    CHECK(IsAllowedPeerAddress({10, 253, 9, 9}));
    CHECK(!ToString(DecodeStatus::BadMagic).empty());
}

void TwoInstancesSameGuestPortsRoundTrip() {
    Env env{A, B};
    auto a = env.Open(0);
    auto b = env.Open(1);
    CHECK_EQ(a->Bind(Addr(Any, 5000)), Errno::SUCCESS);
    CHECK_EQ(b->Bind(Addr(B, 5000)), Errno::SUCCESS); // same guest port, different instance
    a->SetRcvTimeo(2000);
    b->SetRcvTimeo(2000);

    // Bind requests carry exactly op/socket/port, and the named socket really exists.
    const auto requests = env.broker->Requests();
    CHECK_EQ(requests.size(), 2u);
    CHECK_EQ(requests[0].instance, 0u);
    CHECK_EQ(requests[1].instance, 1u);
    for (const auto& request : requests) {
        const auto body = nlohmann::json::parse(request.line);
        CHECK_EQ(body.size(), 3u);
        CHECK_EQ(body.at("op").get<std::string>(), std::string{"bind"});
        CHECK_EQ(body.at("port").get<int>(), 5000);
        const std::string path = body.at("socket").get<std::string>();
        CHECK_EQ(path.rfind(env.dir + "/eln-", 0), 0u);
        struct stat info{};
        CHECK(::stat(path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode));
    }
    CHECK(SocketPathOf(requests[0]) != SocketPathOf(requests[1]));

    CHECK_EQ(Send(*a, Text("hello"), Addr(B, 5000)), (std::pair<s32, Errno>{5, Errno::SUCCESS}));
    CHECK(env.broker->WaitForFramesHandled(1, 2s));
    CHECK_EQ(env.broker->Frames().at(0).bytes, HelloFrame);

    auto first = Receive(*b, 64);
    CHECK_EQ(first.error, Errno::SUCCESS);
    CHECK_EQ(first.data, Text("hello"));
    CHECK_ADDR(first.from, A, 5000);
    CHECK(first.untouched);

    // Reply to the reported source address: identity survives the round trip.
    CHECK_EQ(Send(*b, Text("world!"), first.from), (std::pair<s32, Errno>{6, Errno::SUCCESS}));
    const Bytes expected_reply{0x45, 0x4C, 0x4E, 0x31, 0x01, 0, 0, 0, 0x0A, 0xFD, 0,    3,
                               0x0A, 0xFD, 0,    2,    0x13, 0x88, 0x13, 0x88, 0, 0, 0, 6,
                               'w',  'o',  'r',  'l',  'd',  '!'};
    CHECK(env.broker->WaitForFramesHandled(2, 2s));
    CHECK_EQ(env.broker->Frames().at(1).bytes, expected_reply);
    auto second = Receive(*a, 64);
    CHECK_EQ(second.data, Text("world!"));
    CHECK_ADDR(second.from, B, 5000);

    // A second socket of instance A on another guest port is seen as a different source.
    auto c = env.Open(0);
    CHECK_EQ(c->Bind(Addr(A, 6000)), Errno::SUCCESS);
    CHECK_EQ(Send(*c, Text("x"), Addr(B, 5000)).second, Errno::SUCCESS);
    auto third = Receive(*b, 64);
    CHECK_EQ(third.data, Text("x"));
    CHECK_ADDR(third.from, A, 6000);

    // Both instances report their own virtual address and guest port.
    CHECK_ADDR(a->GetSockName().first, A, 5000);
    CHECK_ADDR(b->GetSockName().first, B, 5000);
    CHECK_EQ(a->GetSockName().second, Errno::SUCCESS);

    // Sending to one's own virtual endpoint loops back through the broker like any peer.
    CHECK_EQ(Send(*a, Text("self"), Addr(A, 5000)).second, Errno::SUCCESS);
    const auto looped = Receive(*a, 16);
    CHECK_EQ(looped.data, Text("self"));
    CHECK_ADDR(looped.from, A, 5000);
    env.ExpectCleanBroker();
}

// The bind request is JSON built from a filesystem path, so awkward directory names must be
// escaped exactly, and a path JSON cannot represent must be refused rather than altered.
void JsonRequestsEscapePaths() {
    Env quoted{{A}, "/tmp/eln-q\"\\\xc3\xa9\t-XXXXXX"}; // quote, backslash, e-acute, tab
    {
        auto a = quoted.Open(0);
        CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::SUCCESS);
        const auto requests = quoted.broker->Requests();
        CHECK_EQ(requests.size(), 1u);
        CHECK_EQ(requests[0].line.find('\n'), std::string::npos);
        const std::string path = SocketPathOf(requests[0]);
        CHECK_EQ(path.rfind(quoted.dir + "/eln-", 0), 0u);
        struct stat info{};
        CHECK(::stat(path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode)); // names the real file
        CHECK(quoted.broker->IsRegistered(A, 5000));
        CHECK_EQ(a->Close(), Errno::SUCCESS);
        CHECK(!quoted.broker->IsRegistered(A, 5000));
        CHECK(quoted.SocketFiles().empty());
    }
    quoted.ExpectCleanBroker();

    Env invalid{{A}, "/tmp/eln-\xff-XXXXXX"}; // not valid UTF-8, so not representable in JSON
    {
        auto a = invalid.Open(0);
        CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::INVAL);
        CHECK_EQ(Send(*a, Text("x"), Addr(B, 5000)).second, Errno::INVAL);
        CHECK_EQ(invalid.broker->Requests().size(), 0u);
        CHECK_ADDR(a->GetSockName().first, Any, 0);
        CHECK_EQ(a->Close(), Errno::SUCCESS);
        CHECK(invalid.SocketFiles().empty());
    }
    invalid.ExpectCleanBroker();
}

void ServicesAndPayloadLimits() {
    Env env{A, B};
    auto a = env.Open(0);
    auto b = env.Open(1);
    CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::SUCCESS);
    CHECK_EQ(b->Bind(Addr(B, 5000)), Errno::SUCCESS);
    b->SetRcvTimeo(2000);

    // Largest legal datagram: 65483 bytes of payload in a 65507 byte frame (one UDP datagram).
    const Bytes big = Pattern(65483);
    CHECK_EQ(Send(*a, big, Addr(B, 5000)), (std::pair<s32, Errno>{65483, Errno::SUCCESS}));
    auto received = Receive(*b, 65483);
    CHECK_EQ(received.size, 65483);
    CHECK_EQ(received.data, big);
    CHECK_EQ(env.broker->Frames().at(0).bytes.size(), 65507u);

    // One byte more is refused locally; nothing reaches the broker.
    CHECK_EQ(Send(*a, Pattern(65484), Addr(B, 5000)), (std::pair<s32, Errno>{-1, Errno::MSGSIZE}));
    CHECK(env.broker->WaitForFramesHandled(1, 2s));
    CHECK_EQ(env.broker->Frames().size(), 1u);

    // Broker services are valid destinations.
    CHECK_EQ(Send(*a, Text("dns"), Addr(ServiceOne, 53)).second, Errno::SUCCESS);
    CHECK_EQ(Send(*a, Text("ntp"), Addr(ServiceTwo, 123)).second, Errno::SUCCESS);
    CHECK(env.broker->WaitForFramesHandled(3, 2s));
    const auto frames = env.broker->Frames();
    const auto dns = Mock::OracleDecode(frames.at(1).bytes);
    const auto ntp = Mock::OracleDecode(frames.at(2).bytes);
    CHECK(dns && ntp);
    CHECK_EQ(dns->destination, ServiceOne);
    CHECK_EQ(dns->destination_port, 53u);
    CHECK_EQ(dns->source, A);
    CHECK_EQ(dns->source_port, 5000u);
    CHECK_EQ(ntp->destination, ServiceTwo);
    CHECK_EQ(ntp->destination_port, 123u);
    env.ExpectCleanBroker();
}

void ZeroLengthTruncationAndPeek() {
    Env env{A, B};
    auto a = env.Open(0);
    auto b = env.Open(1);
    CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::SUCCESS);
    CHECK_EQ(b->Bind(Addr(B, 5000)), Errno::SUCCESS);
    b->SetRcvTimeo(2000);

    // Zero-length datagram: header-only frame on the wire, 0 bytes (success) at the receiver.
    CHECK_EQ(Send(*a, {}, Addr(B, 5000)), (std::pair<s32, Errno>{0, Errno::SUCCESS}));
    const Bytes empty_frame{0x45, 0x4C, 0x4E, 0x31, 1, 0, 0, 0, 10, 253, 0, 2,
                            10,   253,  0,    3,    0x13, 0x88, 0x13, 0x88, 0, 0, 0, 0};
    CHECK(env.broker->WaitForFramesHandled(1, 2s));
    CHECK_EQ(env.broker->Frames().at(0).bytes, empty_frame);
    auto zero = Receive(*b, 16);
    CHECK_EQ(zero.size, 0);
    CHECK_EQ(zero.error, Errno::SUCCESS);
    CHECK_ADDR(zero.from, A, 5000);
    CHECK(zero.untouched);

    // Truncation behaves like native UDP: prefix returned, rest discarded, no error.
    CHECK_EQ(Send(*a, Text("0123456789"), Addr(B, 5000)).second, Errno::SUCCESS);
    auto cut = Receive(*b, 4);
    CHECK_EQ(cut.error, Errno::SUCCESS);
    CHECK_EQ(cut.data, Text("0123"));
    CHECK(cut.untouched);
    b->SetNonBlock(true);
    CHECK_EQ(Receive(*b, 64).error, Errno::AGAIN); // the truncated datagram is gone
    b->SetNonBlock(false);

    // MSG_PEEK never consumes, and reports the same truncation rules.
    CHECK_EQ(Send(*a, Text("abcdef"), Addr(B, 5000)).second, Errno::SUCCESS);
    auto peek_short = Receive(*b, 3, static_cast<int>(FLAG_MSG_PEEK));
    CHECK_EQ(peek_short.data, Text("abc"));
    auto peek_full = Receive(*b, 64, static_cast<int>(FLAG_MSG_PEEK));
    CHECK_EQ(peek_full.data, Text("abcdef"));
    CHECK_ADDR(peek_full.from, A, 5000);
    auto taken = Receive(*b, 64);
    CHECK_EQ(taken.data, Text("abcdef"));
    b->SetNonBlock(true);
    CHECK_EQ(Receive(*b, 64).error, Errno::AGAIN);

    // A zero-sized buffer still consumes the datagram and returns 0.
    b->SetNonBlock(false);
    CHECK_EQ(Send(*a, Text("zz"), Addr(B, 5000)).second, Errno::SUCCESS);
    auto none = Receive(*b, 0);
    CHECK_EQ(none.size, 0);
    CHECK_EQ(none.error, Errno::SUCCESS);
    b->SetNonBlock(true);
    CHECK_EQ(Receive(*b, 64).error, Errno::AGAIN);
    env.ExpectCleanBroker();
}

void PollBlockingNonBlockingAndTimeouts() {
    Env env{A, B};
    auto a = env.Open(0);
    auto b = env.Open(1);
    CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::SUCCESS);
    CHECK_EQ(b->Bind(Addr(B, 5000)), Errno::SUCCESS);

    // The descriptor is a real, close-on-exec datagram Unix socket.
    int value = 0;
    socklen_t length = sizeof(value);
    CHECK(::getsockopt(b->GetFD(), SOL_SOCKET, SO_DOMAIN, &value, &length) == 0 && value == AF_UNIX);
    CHECK(::getsockopt(b->GetFD(), SOL_SOCKET, SO_TYPE, &value, &length) == 0 && value == SOCK_DGRAM);
    CHECK((::fcntl(b->GetFD(), F_GETFD) & FD_CLOEXEC) != 0);

    // Poll timeout with nothing queued.
    {
        Stopwatch watch;
        std::vector<PollFD> fds{PollFD{b.get(), PollEvents::In, PollEvents{}}};
        const auto [count, error] = Poll(fds, 120);
        CHECK_EQ(count, 0);
        CHECK_EQ(error, Errno::SUCCESS);
        CHECK(watch.Ms() >= 100.0 && watch.Ms() < 3000.0);
        CHECK_EQ(static_cast<u16>(fds[0].revents), static_cast<u16>(0));
    }
    // Writable immediately.
    {
        std::vector<PollFD> fds{PollFD{b.get(), PollEvents::Out, PollEvents{}}};
        const auto [count, error] = Poll(fds, 1000);
        CHECK_EQ(count, 1);
        CHECK((static_cast<u16>(fds[0].revents) & static_cast<u16>(PollEvents::Out)) != 0);
    }
    // Readiness appears when a datagram arrives and disappears once it is received.
    CHECK_EQ(Send(*a, Text("ping"), Addr(B, 5000)).second, Errno::SUCCESS);
    CHECK(Readable(*b, 2000));
    CHECK(Readable(*b, 0)); // level triggered until consumed
    CHECK_EQ(Receive(*b, 16).data, Text("ping"));
    CHECK(!Readable(*b, 0));

    // Non-blocking mode (the BSD layer's SetNonBlock) and the per-call DONTWAIT flag.
    CHECK_EQ(b->SetNonBlock(true), Errno::SUCCESS);
    CHECK((::fcntl(b->GetFD(), F_GETFL) & O_NONBLOCK) != 0);
    {
        Stopwatch watch;
        CHECK_EQ(Receive(*b, 16).error, Errno::AGAIN);
        CHECK(watch.Ms() < 1000.0);
    }
    CHECK_EQ(b->SetNonBlock(false), Errno::SUCCESS);
    CHECK((::fcntl(b->GetFD(), F_GETFL) & O_NONBLOCK) == 0);
    {
        Stopwatch watch;
        CHECK_EQ(Receive(*b, 16, static_cast<int>(FLAG_MSG_DONTWAIT)).error, Errno::AGAIN);
        CHECK(watch.Ms() < 1000.0);
    }
    // DONTWAIT combined with PEEK on a queued datagram.
    CHECK_EQ(Send(*a, Text("pk"), Addr(B, 5000)).second, Errno::SUCCESS);
    CHECK(Readable(*b, 2000));
    CHECK_EQ(Receive(*b, 16, static_cast<int>(FLAG_MSG_DONTWAIT | FLAG_MSG_PEEK)).data, Text("pk"));
    CHECK_EQ(Receive(*b, 16, static_cast<int>(FLAG_MSG_DONTWAIT)).data, Text("pk"));

    // Receive timeout: native EAGAIN after about the configured time, without spinning.
    CHECK_EQ(b->SetRcvTimeo(250), Errno::SUCCESS);
    {
        Stopwatch watch;
        const double cpu_before = ThreadCpuMs();
        CHECK_EQ(Receive(*b, 16).error, Errno::AGAIN);
        const double cpu = ThreadCpuMs() - cpu_before;
        CHECK(watch.Ms() >= 200.0 && watch.Ms() < 3000.0);
        CHECK(cpu < 50.0);
    }
    // Real SO_RCVTIMEO value: 2500 ms is 2 s + 500000 us, checked on the raw descriptor.
    CHECK_EQ(b->SetRcvTimeo(2500), Errno::SUCCESS);
    CHECK_EQ(b->SetSndTimeo(1250), Errno::SUCCESS);
    timeval tv{};
    length = sizeof(tv);
    CHECK(::getsockopt(b->GetFD(), SOL_SOCKET, SO_RCVTIMEO, &tv, &length) == 0);
    CHECK_EQ(static_cast<long>(tv.tv_sec), 2L);
    CHECK_EQ(static_cast<long>(tv.tv_usec), 500000L);
    CHECK(::getsockopt(b->GetFD(), SOL_SOCKET, SO_SNDTIMEO, &tv, &length) == 0);
    CHECK_EQ(static_cast<long>(tv.tv_sec), 1L);
    CHECK_EQ(static_cast<long>(tv.tv_usec), 250000L);

    // A blocked receive wakes when data arrives from another thread, again without spinning.
    CHECK_EQ(b->SetRcvTimeo(5000), Errno::SUCCESS);
    std::thread sender([&] {
        std::this_thread::sleep_for(300ms);
        (void)Send(*a, Text("late"), Addr(B, 5000));
    });
    Stopwatch watch;
    const double cpu_before = ThreadCpuMs();
    auto late = Receive(*b, 16);
    const double cpu = ThreadCpuMs() - cpu_before;
    sender.join();
    CHECK_EQ(late.data, Text("late"));
    CHECK(watch.Ms() >= 250.0 && watch.Ms() < 3000.0);
    CHECK(cpu < 50.0);

    // Unsupported flags are refused rather than ignored.
    CHECK_EQ(Receive(*b, 16, 0x1000).error, Errno::INVAL);
    CHECK_EQ(Receive(*b, 16, 0x1).error, Errno::INVAL);
    CHECK_EQ(a->SendTo(0x1, Text("f"), nullptr).second, Errno::NOTCONN); // no peer yet
    CHECK_EQ(Send(*a, Text("f"), Addr(B, 5000), 0x1).second, Errno::INVAL);
    CHECK_EQ(Send(*a, Text("f"), Addr(B, 5000), 0x100).second, Errno::INVAL);
    // MSG_DONTWAIT on send is accepted and the datagram is delivered.
    CHECK_EQ(Send(*a, Text("nb"), Addr(B, 5000), FLAG_MSG_DONTWAIT).second, Errno::SUCCESS);
    CHECK(Readable(*b, 2000));
    CHECK_EQ(Receive(*b, 16).data, Text("nb"));

    // Inherited native option setters work on the real descriptor.
    CHECK_EQ(b->SetReuseAddr(true), Errno::SUCCESS);
    CHECK_EQ(b->SetBroadcast(true), Errno::SUCCESS);
    CHECK_EQ(b->SetKeepAlive(true), Errno::SUCCESS);
    CHECK_EQ(b->SetSndBuf(65536), Errno::SUCCESS);
    CHECK_EQ(b->SetRcvBuf(65536), Errno::SUCCESS);
    CHECK_EQ(b->SetLinger(true, 1), Errno::SUCCESS);
    const auto pending = b->GetPendingError();
    CHECK_EQ(pending.first, Errno::SUCCESS);
    CHECK_EQ(pending.second, Errno::SUCCESS);
    env.ExpectCleanBroker();
}

void MalformedDatagramsAreErrorsNotPayload() {
    Env env{A, B};
    auto b = env.Open(1);
    CHECK_EQ(b->Bind(Addr(B, 5000)), Errno::SUCCESS);
    b->SetNonBlock(true);
    CHECK_EQ(env.broker->Requests().size(), 1u);
    const std::string path = SocketPathOf(env.broker->Requests()[0]);
    TestSupport::ClearLog();

    const Bytes valid = Mock::OracleEncode(A, B, 4000, 5000, Text("good"));
    struct Case {
        const char* name;
        Bytes bytes;
    };
    std::vector<Case> cases;
    const auto mutate = [&](const char* name, std::function<void(Bytes&)> edit) {
        Bytes copy = valid;
        edit(copy);
        cases.push_back({name, std::move(copy)});
    };
    mutate("bad magic", [](Bytes& b) { b[0] = 'X'; });
    mutate("version 2", [](Bytes& b) { b[4] = 2; });
    mutate("type 0", [](Bytes& b) { b[4] = 0; });
    mutate("reserved 5", [](Bytes& b) { b[5] = 1; });
    mutate("reserved 6", [](Bytes& b) { b[6] = 1; });
    mutate("reserved 7", [](Bytes& b) { b[7] = 1; });
    mutate("short 23", [](Bytes& b) { b.resize(23); });
    mutate("empty", [](Bytes& b) { b.clear(); });
    mutate("length too big", [](Bytes& b) { b[23] = 9; });
    mutate("length too small", [](Bytes& b) { b[23] = 2; });
    mutate("trailing byte", [](Bytes& b) { b.push_back(0); });
    mutate("length 65484", [](Bytes&) {});
    cases.back().bytes = Mock::OracleEncode(A, B, 4000, 5000, Bytes(65483));
    cases.back().bytes.push_back(0);
    cases.back().bytes[23] = 0xCC;
    mutate("datagram beyond the buffer", [](Bytes&) {});
    cases.back().bytes = Mock::OracleEncode(A, B, 4000, 5000, Bytes(65483 + 40));
    mutate("destination port", [](Bytes& b) { b[19] = 0x89; });
    mutate("destination ip", [](Bytes& b) { b[15] = 4; });
    mutate("source port 0", [](Bytes& b) { b[16] = b[17] = 0; });
    mutate("source public", [](Bytes& b) { b[8] = 8; b[9] = 8; b[10] = 8; b[11] = 8; });
    mutate("source loopback", [](Bytes& b) { b[8] = 127; b[9] = 0; b[10] = 0; b[11] = 1; });
    mutate("source 10.254.0.3", [](Bytes& b) { b[9] = 254; b[10] = 0; b[11] = 3; });
    mutate("source network address", [](Bytes& b) { b[10] = 0; b[11] = 0; });
    mutate("source broadcast address", [](Bytes& b) { b[10] = 255; b[11] = 255; });
    mutate("source 0.0.0.0", [](Bytes& b) { b[8] = b[9] = b[10] = b[11] = 0; });

    for (const auto& item : cases) {
        CHECK_EQ(Mock::Broker::Inject(path, item.bytes), 0);
        CHECK(Readable(*b, 1000));
        const auto result = Receive(*b, 128);
        if (result.error != Errno::INVAL || result.size != -1 || !result.untouched) {
            Fail(__FILE__, __LINE__,
                 std::string{"case '"} + item.name + "' returned " + Show(result.error) + "/" +
                     std::to_string(result.size) + (result.untouched ? "" : " and wrote output"));
        }
        // The bad datagram was consumed: the queue is empty and the next one is delivered.
        CHECK_EQ(Receive(*b, 128).error, Errno::AGAIN);
    }
    CHECK(TestSupport::LogContains(Common::Log::Level::Warning, "Rejected LAN datagram"));

    // Peeking at a malformed datagram reports the error but leaves it for the real receive.
    CHECK_EQ(Mock::Broker::Inject(path, cases[0].bytes), 0);
    CHECK_EQ(Receive(*b, 128, static_cast<int>(FLAG_MSG_PEEK)).error, Errno::INVAL);
    CHECK(Readable(*b, 0));
    CHECK_EQ(Receive(*b, 128).error, Errno::INVAL);
    CHECK(!Readable(*b, 0));

    // A bad datagram does not poison what follows it.
    CHECK_EQ(Mock::Broker::Inject(path, cases[0].bytes), 0);
    CHECK_EQ(Mock::Broker::Inject(path, valid), 0);
    CHECK_EQ(Receive(*b, 128).error, Errno::INVAL);
    const auto good = Receive(*b, 128);
    CHECK_EQ(good.data, Text("good"));
    CHECK_ADDR(good.from, A, 4000);

    // Every legitimate source form is delivered: peers at both ends of 10.253/16 and services.
    for (const Ip& source : {Ip{10, 253, 0, 1}, Ip{10, 253, 255, 254}, ServiceOne, ServiceTwo, B}) {
        CHECK_EQ(Mock::Broker::Inject(path, Mock::OracleEncode(source, B, 65535, 5000, Text("ok"))),
                 0);
        const auto delivered = Receive(*b, 16);
        CHECK_EQ(delivered.data, Text("ok"));
        CHECK_ADDR(delivered.from, source, 65535);
    }
    env.ExpectCleanBroker();
}

void BindRulesAndBrokerFailures() {
    Env env{A, B};
    auto a = env.Open(0);

    // Local rejections never bother the broker.
    CHECK_EQ(a->Bind(Addr({8, 8, 8, 8}, 5000)), Errno::ADDRNOTAVAIL);
    CHECK_EQ(a->Bind(Addr(B, 5000)), Errno::ADDRNOTAVAIL); // someone else's virtual address
    CHECK_EQ(a->Bind(SockAddrIn{.family = Domain::Unspecified, .ip = Any, .portno = 5000}), Errno::INVAL);
    CHECK_EQ(env.broker->Requests().size(), 0u);
    CHECK_ADDR(a->GetSockName().first, Any, 0);

    // Explicit bind keeps the requested port; rebinding is refused without a second request.
    CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::SUCCESS);
    CHECK_ADDR(a->GetSockName().first, A, 5000);
    CHECK_EQ(a->Bind(Addr(A, 5001)), Errno::INVAL);
    CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::INVAL);
    CHECK_EQ(env.broker->Requests().size(), 1u);
    CHECK_ADDR(a->GetSockName().first, A, 5000);

    // Broker refusal (genuine duplicate): error, nothing bound, and the socket can retry.
    auto dup = env.Open(0);
    TestSupport::ClearLog();
    CHECK_EQ(dup->Bind(Addr(Any, 5000)), Errno::ADDRINUSE);
    CHECK(TestSupport::LogContains(Common::Log::Level::Error, "refused bind: port_in_use"));
    CHECK_ADDR(dup->GetSockName().first, Any, 0);
    CHECK_EQ(dup->Bind(Addr(Any, 5001)), Errno::SUCCESS);
    CHECK_ADDR(dup->GetSockName().first, A, 5001);

    // Port 0 lets the broker choose a nonzero port.
    auto free_port = env.Open(0);
    CHECK_EQ(free_port->Bind(Addr(Any, 0)), Errno::SUCCESS);
    const auto chosen = free_port->GetSockName().first;
    CHECK(chosen.portno != 0);
    CHECK_EQ(chosen.portno, static_cast<u16>(49152));
    CHECK_EQ(nlohmann::json::parse(env.broker->Requests().back().line).at("port").get<int>(), 0);

    // Responses the guest must not believe. Each leaves the socket unbound and returns OTHER.
    const std::vector<std::pair<const char*, std::string>> bad_replies{
        {"not json", "garbage\n"},
        {"array", "[]\n"},
        {"empty line", "\n"},
        {"ok not boolean", "{\"ok\":\"yes\",\"port\":5000}\n"},
        {"ok missing", "{\"port\":5000}\n"},
        {"refused without text", "{\"ok\":false}\n"},
        {"port missing", "{\"ok\":true}\n"},
        {"port zero", "{\"ok\":true,\"port\":0}\n"},
        {"port too large", "{\"ok\":true,\"port\":65536}\n"},
        {"port negative", "{\"ok\":true,\"port\":-1}\n"},
        {"port fractional", "{\"ok\":true,\"port\":5000.5}\n"},
        {"port float form", "{\"ok\":true,\"port\":5000.0}\n"},
        {"port string", "{\"ok\":true,\"port\":\"5000\"}\n"},
        {"truncated, no newline", "{\"ok\":true,\"port\":5000}"},
        {"empty reply", ""},
    };
    for (const auto& [name, reply] : bad_replies) {
        auto victim = env.Open(1);
        env.broker->ScriptReply(1, reply);
        const Errno result = victim->Bind(Addr(Any, 7000));
        if (result != Errno::OTHER) {
            Fail(__FILE__, __LINE__, std::string{"bad reply '"} + name + "' gave " + Show(result));
        }
        CHECK_ADDR(victim->GetSockName().first, Any, 0);
    }

    // The broker answering a different port than requested is a refusal, and the guest undoes
    // the registration it now believes exists.
    {
        auto victim = env.Open(1);
        const auto before = env.broker->Requests().size();
        env.broker->ScriptReply(1, "{\"ok\":true,\"port\":5000}\n");
        CHECK_EQ(victim->Bind(Addr(Any, 6000)), Errno::OTHER);
        CHECK_ADDR(victim->GetSockName().first, Any, 0);
        const auto requests = env.broker->Requests();
        CHECK_EQ(requests.size(), before + 2);
        CHECK_EQ(nlohmann::json::parse(requests[before + 1].line).at("op").get<std::string>(),
                 std::string{"unbind"});
        CHECK_EQ(SocketPathOf(requests[before + 1]), SocketPathOf(requests[before]));
    }

    // Response size limit: 4096 bytes including the newline is accepted, one more is not.
    const std::string head = "{\"ok\":true,\"port\":5000,\"pad\":\"";
    const std::string tail = "\"}";
    const auto padded = [&](std::size_t total) {
        return head + std::string(total - head.size() - tail.size() - 1, 'x') + tail + "\n";
    };
    CHECK_EQ(padded(4096).size(), 4096u);
    {
        auto at_limit = env.Open(1);
        env.broker->ScriptReply(1, padded(4096));
        CHECK_EQ(at_limit->Bind(Addr(Any, 5000)), Errno::SUCCESS);
        CHECK_ADDR(at_limit->GetSockName().first, B, 5000);
        auto over_limit = env.Open(1);
        env.broker->ScriptReply(1, padded(4097));
        CHECK_EQ(over_limit->Bind(Addr(Any, 5001)), Errno::OTHER);
        auto huge = env.Open(1);
        env.broker->ScriptReply(1, std::string(100000, 'x'));
        CHECK_EQ(huge->Bind(Addr(Any, 5002)), Errno::OTHER);
    }
    env.ExpectCleanBroker();
}

// The refusal codes the Go broker actually emits (control.go) map to distinct errnos; anything
// else, including a missing code, stays OTHER. None of them leaves the socket bound.
void BrokerRefusalCodesMapToErrno() {
    Env env{A};
    const std::vector<std::pair<std::string, Errno>> codes{
        {"port_in_use", Errno::ADDRINUSE}, {"forbidden", Errno::ACCES},
        {"not_ready", Errno::NETDOWN},     {"already_bound", Errno::INVAL},
        {"bad_path", Errno::OTHER},        {"not_socket", Errno::OTHER},
        {"bad_request", Errno::OTHER},     {"too_large", Errno::OTHER},
        {"unknown_op", Errno::OTHER},      {"something new", Errno::OTHER},
    };
    for (const auto& [code, expected] : codes) {
        auto socket = env.Open(0);
        env.broker->ScriptReply(0, "{\"ok\":false,\"error\":\"" + code + "\"}\n");
        const Errno result = socket->Bind(Addr(A, 5000));
        if (result != expected) {
            Fail(__FILE__, __LINE__, "refusal '" + code + "' gave " + Show(result) + ", expected " + Show(expected));
        }
        CHECK_ADDR(socket->GetSockName().first, Any, 0);
    }
    // Implicit bind surfaces the same errno.
    auto implicit = env.Open(0);
    env.broker->ScriptReply(0, "{\"ok\":false,\"error\":\"port_in_use\"}\n");
    CHECK_EQ(Send(*implicit, Text("x"), Addr(B, 5000)), (std::pair<s32, Errno>{-1, Errno::ADDRINUSE}));
    env.ExpectCleanBroker();
}

void BrokerUnavailableOrSilent() {
    Env env{A, B};

    // A broker that reads the request and never answers: bounded by about 2 seconds.
    {
        auto a = env.Open(0);
        env.broker->ScriptHang(0);
        Stopwatch watch;
        CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::TIMEDOUT);
        CHECK(watch.Ms() >= 1900.0 && watch.Ms() < 3500.0);
        CHECK_ADDR(a->GetSockName().first, Any, 0);
        // The socket is still usable once the broker behaves.
        CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::SUCCESS);
    }

    // Close with a silent broker is also bounded and still cleans up locally.
    {
        auto a = env.Open(0);
        CHECK_EQ(a->Bind(Addr(A, 5100)), Errno::SUCCESS);
        const auto files_before = env.SocketFiles().size();
        env.broker->ScriptHang(0); // the unbind
        Stopwatch watch;
        CHECK_EQ(a->Close(), Errno::SUCCESS);
        CHECK(watch.Ms() >= 400.0 && watch.Ms() < 1900.0);
        CHECK(!a->IsOpened());
        CHECK_EQ(env.SocketFiles().size(), files_before - 1);
    }

    // No control socket at all, a stale control socket, and no broker datagram endpoint.
    const std::string absent = env.dir + "/absent-control.sock";
    NsoLanSocket missing{absent, env.dir + "/absent-data.sock", A};
    CHECK_EQ(missing.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP), Errno::SUCCESS);
    CHECK_EQ(missing.Bind(Addr(A, 5000)), Errno::NETDOWN);
    CHECK_EQ(missing.Connect(Addr(B, 5000)), Errno::NETDOWN);
    CHECK_EQ(missing.GetPeerName().second, Errno::NOTCONN);
    CHECK_EQ(Send(missing, Text("x"), Addr(B, 5000)), (std::pair<s32, Errno>{-1, Errno::NETDOWN}));
    CHECK_ADDR(missing.GetSockName().first, Any, 0);

    const std::string stale = env.dir + "/stale-control.sock";
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, stale.c_str(), stale.size());
        CHECK(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        ::close(fd); // leaves the socket file behind with nobody listening
    }
    NsoLanSocket refused{stale, env.dir + "/absent-data.sock", A};
    CHECK_EQ(refused.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP), Errno::SUCCESS);
    CHECK_EQ(refused.Bind(Addr(A, 5000)), Errno::CONNREFUSED);

    // Registered fine, but the broker's datagram endpoint is gone or dead.
    NsoLanSocket no_endpoint{env.broker->ControlPath(0), env.dir + "/absent-data.sock", A};
    CHECK_EQ(no_endpoint.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP), Errno::SUCCESS);
    CHECK_EQ(no_endpoint.Bind(Addr(A, 5200)), Errno::SUCCESS);
    CHECK_EQ(Send(no_endpoint, Text("x"), Addr(B, 5000)), (std::pair<s32, Errno>{-1, Errno::NETDOWN}));
    const std::string dead = env.dir + "/dead-data.sock";
    {
        const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, dead.c_str(), dead.size());
        CHECK(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        ::close(fd);
    }
    NsoLanSocket dead_endpoint{env.broker->ControlPath(0), dead, A};
    CHECK_EQ(dead_endpoint.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP), Errno::SUCCESS);
    CHECK_EQ(dead_endpoint.Bind(Addr(A, 5201)), Errno::SUCCESS);
    CHECK_EQ(Send(dead_endpoint, Text("x"), Addr(B, 5000)),
             (std::pair<s32, Errno>{-1, Errno::CONNREFUSED}));
    env.ExpectCleanBroker();
}

void AutoBindAndConnectedSockets() {
    Env env{A, B};
    auto a = env.Open(0);
    auto b = env.Open(1);
    auto other = env.Open(1);
    CHECK_EQ(b->Bind(Addr(B, 7000)), Errno::SUCCESS);
    CHECK_EQ(other->Bind(Addr(B, 7001)), Errno::SUCCESS);
    a->SetRcvTimeo(2000);
    b->SetRcvTimeo(2000);
    other->SetRcvTimeo(2000);
    const auto binds_before = env.broker->Requests().size();

    // Not connected, not bound: nothing to send to, and no registration was attempted.
    CHECK_EQ(a->Send(Text("x"), 0), (std::pair<s32, Errno>{-1, Errno::NOTCONN}));
    CHECK_EQ(a->SendTo(0, Text("x"), nullptr), (std::pair<s32, Errno>{-1, Errno::NOTCONN}));
    CHECK_EQ(a->GetPeerName().second, Errno::NOTCONN);
    CHECK_EQ(env.broker->Requests().size(), binds_before);

    // Connect registers guest port 0 (the broker picks) and caches the peer.
    CHECK_EQ(a->Connect(Addr(B, 7000)), Errno::SUCCESS);
    const auto requests = env.broker->Requests();
    CHECK_EQ(requests.size(), binds_before + 1);
    const auto auto_bind = nlohmann::json::parse(requests.back().line);
    CHECK_EQ(auto_bind.at("op").get<std::string>(), std::string{"bind"});
    CHECK_EQ(auto_bind.at("port").get<int>(), 0);
    const auto name = a->GetSockName();
    CHECK_EQ(name.second, Errno::SUCCESS);
    CHECK_ADDR(name.first, A, 49152);
    const auto peer = a->GetPeerName();
    CHECK_EQ(peer.second, Errno::SUCCESS);
    CHECK_ADDR(peer.first, B, 7000);

    // Send uses the cached peer; the frame carries the auto-bound port.
    CHECK_EQ(a->Send(Text("hi"), 0), (std::pair<s32, Errno>{2, Errno::SUCCESS}));
    const Bytes expected{0x45, 0x4C, 0x4E, 0x31, 1, 0, 0, 0, 10, 253, 0, 2, 10, 253, 0, 3,
                         0xC0, 0x00, 0x1B, 0x58, 0, 0, 0, 2, 'h', 'i'}; // 49152 -> 7000
    CHECK(env.broker->WaitForFramesHandled(1, 2s));
    CHECK_EQ(env.broker->Frames().at(0).bytes, expected);
    CHECK_EQ(Receive(*b, 16).data, Text("hi"));
    // An explicit address still works on a connected socket, as for native UDP.
    CHECK_EQ(Send(*a, Text("yo"), Addr(B, 7001)).second, Errno::SUCCESS);
    CHECK_EQ(Receive(*other, 16).data, Text("yo"));

    // A connected socket only hears from its peer. `other` speaks first and is ignored.
    CHECK_EQ(Send(*other, Text("intruder"), Addr(A, 49152)).second, Errno::SUCCESS);
    CHECK_EQ(Send(*b, Text("friend"), Addr(A, 49152)).second, Errno::SUCCESS);
    CHECK(env.broker->WaitForFramesHandled(4, 2s));
    const auto heard = Receive(*a, 32);
    CHECK_EQ(heard.data, Text("friend"));
    CHECK_ADDR(heard.from, B, 7000);
    a->SetNonBlock(true);
    CHECK_EQ(Receive(*a, 32).error, Errno::AGAIN);
    // Same through MSG_PEEK: the stranger is discarded, the peer's datagram is only peeked.
    CHECK_EQ(Send(*other, Text("intruder2"), Addr(A, 49152)).second, Errno::SUCCESS);
    CHECK_EQ(Send(*b, Text("friend2"), Addr(A, 49152)).second, Errno::SUCCESS);
    CHECK(env.broker->WaitForFramesHandled(6, 2s));
    CHECK(Readable(*a, 2000));
    CHECK_EQ(Receive(*a, 32, static_cast<int>(FLAG_MSG_PEEK)).data, Text("friend2"));
    CHECK_EQ(Receive(*a, 32).data, Text("friend2"));
    CHECK_EQ(Receive(*a, 32).error, Errno::AGAIN);
    // A stranger alone: non-blocking receive reports an empty queue, not the stranger's bytes.
    CHECK_EQ(Send(*other, Text("alone"), Addr(A, 49152)).second, Errno::SUCCESS);
    CHECK(env.broker->WaitForFramesHandled(7, 2s));
    CHECK_EQ(Receive(*a, 32).error, Errno::AGAIN);
    CHECK_EQ(Receive(*a, 32, static_cast<int>(FLAG_MSG_PEEK)).error, Errno::AGAIN);

    // Bad connect targets change nothing.
    CHECK_EQ(a->Connect(Addr({8, 8, 8, 8}, 53)), Errno::NETUNREACH);
    CHECK_EQ(a->Connect(Addr(B, 0)), Errno::INVAL);
    CHECK_EQ(a->Connect(SockAddrIn{.family = Domain::Unspecified, .ip = B, .portno = 7000}), Errno::INVAL);
    CHECK_ADDR(a->GetPeerName().first, B, 7000);
    // Reconnecting retargets.
    CHECK_EQ(a->Connect(Addr(B, 7001)), Errno::SUCCESS);
    CHECK_ADDR(a->GetPeerName().first, B, 7001);

    // A failed auto-bind during connect leaves no peer behind and can be retried.
    auto c = env.Open(0);
    env.broker->ScriptReply(0, "{\"ok\":false,\"error\":\"no ports\"}\n");
    CHECK_EQ(c->Connect(Addr(B, 7000)), Errno::OTHER);
    CHECK_EQ(c->GetPeerName().second, Errno::NOTCONN);
    CHECK_ADDR(c->GetSockName().first, Any, 0);
    CHECK_EQ(c->Connect(Addr(B, 7000)), Errno::SUCCESS);
    // And the same for the implicit bind of an unconnected send: nothing is delivered.
    auto d = env.Open(0);
    const auto frames_before = env.broker->Frames().size();
    env.broker->ScriptReply(0, "{\"ok\":false,\"error\":\"no ports\"}\n");
    CHECK_EQ(Send(*d, Text("x"), Addr(B, 7000)), (std::pair<s32, Errno>{-1, Errno::OTHER}));
    CHECK_EQ(Send(*d, Text("x"), Addr(B, 7000)).second, Errno::SUCCESS); // retry binds and sends
    CHECK(env.broker->WaitForFramesHandled(frames_before + 1, 2s));
    CHECK_EQ(env.broker->Frames().size(), frames_before + 1);
    env.ExpectCleanBroker();
}

void ImplicitBindIsRaceFree() {
    Env env{A, B};
    auto a = env.Open(0);
    auto b = env.Open(1);
    CHECK_EQ(b->Bind(Addr(B, 6000)), Errno::SUCCESS);
    constexpr int Threads = 8;
    std::atomic<bool> go{false};
    std::atomic<int> succeeded{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < Threads; ++i) {
        workers.emplace_back([&] {
            go.wait(false);
            if (Send(*a, Text("x"), Addr(B, 6000)).second == Errno::SUCCESS) {
                ++succeeded;
            }
        });
    }
    go = true;
    go.notify_all();
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK_EQ(succeeded.load(), Threads);
    const auto requests = env.broker->Requests();
    CHECK_EQ(requests.size(), 2u); // b's bind and exactly one implicit bind of a
    CHECK(env.broker->WaitForFramesHandled(Threads, 2s));
    for (const auto& frame : env.broker->Frames()) {
        const auto decoded = Mock::OracleDecode(frame.bytes);
        CHECK(decoded.has_value());
        CHECK_EQ(decoded->source_port, 49152u);
    }
    env.ExpectCleanBroker();
}

void DestinationAndBroadcastRules() {
    Env env{A, B};
    auto a = env.Open(0);
    CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::SUCCESS);
    const auto requests_before = env.broker->Requests().size();
    TestSupport::ClearLog();

    const auto expect = [&](const char* label, SockAddrIn to, Errno expected) {
        const auto result = Send(*a, Text("x"), to);
        if (result.first != -1 || result.second != expected) {
            Fail(__FILE__, __LINE__, std::string{label} + " returned " + Show(result.second));
        }
    };
    expect("limited broadcast", Addr({255, 255, 255, 255}, 5000), Errno::NETUNREACH);
    expect("subnet broadcast", Addr({10, 253, 255, 255}, 5000), Errno::NETUNREACH);
    expect("multicast", Addr({224, 0, 0, 1}, 5000), Errno::NETUNREACH);
    expect("multicast top", Addr({239, 255, 255, 250}, 1900), Errno::NETUNREACH);
    expect("public", Addr({8, 8, 8, 8}, 53), Errno::NETUNREACH);
    expect("loopback", Addr({127, 0, 0, 1}, 5000), Errno::NETUNREACH);
    expect("any", Addr(Any, 5000), Errno::NETUNREACH);
    expect("other service", Addr({10, 254, 0, 3}, 5000), Errno::NETUNREACH);
    expect("network address", Addr({10, 253, 0, 0}, 5000), Errno::NETUNREACH);
    expect("lan", Addr({192, 168, 1, 5}, 5000), Errno::NETUNREACH);
    expect("port zero", Addr(B, 0), Errno::INVAL);
    expect("wrong family", SockAddrIn{.family = Domain::Unspecified, .ip = B, .portno = 5000}, Errno::INVAL);
    CHECK(TestSupport::LogContains(Common::Log::Level::Warning, "broadcast or multicast"));

    // Nothing was fabricated: no datagram reached the broker and no registration was made.
    CHECK(!env.broker->WaitForFramesHandled(1, 100ms));
    CHECK_EQ(env.broker->Frames().size(), 0u);
    CHECK_EQ(env.broker->Requests().size(), requests_before);
    // Rejected sends do not even auto-bind an unbound socket.
    auto unbound = env.Open(0);
    CHECK_EQ(Send(*unbound, Text("x"), Addr({255, 255, 255, 255}, 5000)).second, Errno::NETUNREACH);
    CHECK_EQ(env.broker->Requests().size(), requests_before);
    CHECK_ADDR(unbound->GetSockName().first, Any, 0);
    // SO_BROADCAST does not turn broadcast into fake delivery.
    CHECK_EQ(unbound->SetBroadcast(true), Errno::SUCCESS);
    CHECK_EQ(Send(*unbound, Text("x"), Addr({255, 255, 255, 255}, 5000)).second, Errno::NETUNREACH);
    CHECK_EQ(env.broker->Frames().size(), 0u);
    env.ExpectCleanBroker();
}

void InitializeAcceptsOnlyUdpDatagrams() {
    Env env{A};
    const auto init = [&](Domain d, Type t, Protocol p) {
        auto socket = env.Raw(0);
        const Errno result = socket->Initialize(d, t, p);
        CHECK_EQ(socket->IsOpened(), result == Errno::SUCCESS);
        CHECK_EQ(env.SocketFiles().size(), result == Errno::SUCCESS ? 1u : 0u);
        return result;
    };
    CHECK_EQ(init(Domain::INET, Type::DGRAM, Protocol::UDP), Errno::SUCCESS);
    CHECK_EQ(init(Domain::INET, Type::DGRAM, Protocol::Unspecified), Errno::SUCCESS);
    CHECK_EQ(init(Domain::INET, Type::DGRAM, Protocol::IP), Errno::SUCCESS);
    CHECK_EQ(init(Domain::INET, Type::DGRAM, Protocol::TCP), Errno::INVAL);
    CHECK_EQ(init(Domain::INET, Type::DGRAM, Protocol::ICMP), Errno::INVAL);
    CHECK_EQ(init(Domain::INET, Type::DGRAM, Protocol::SCTP), Errno::INVAL);
    CHECK_EQ(init(Domain::INET, Type::STREAM, Protocol::TCP), Errno::INVAL);
    CHECK_EQ(init(Domain::INET, Type::STREAM, Protocol::UDP), Errno::INVAL);
    CHECK_EQ(init(Domain::INET, Type::RAW, Protocol::Unspecified), Errno::INVAL);
    CHECK_EQ(init(Domain::INET, Type::SEQPACKET, Protocol::Unspecified), Errno::INVAL);
    CHECK_EQ(init(Domain::INET, Type::Unspecified, Protocol::UDP), Errno::INVAL);
    CHECK_EQ(init(Domain::Unspecified, Type::DGRAM, Protocol::UDP), Errno::INVAL);

    auto socket = env.Open(0);
    CHECK_EQ(socket->Initialize(Domain::INET, Type::DGRAM, Protocol::UDP), Errno::INVAL); // twice
    CHECK(socket->IsOpened());

    // Every operation on an uninitialized socket is a clean error.
    auto raw = env.Raw(0);
    CHECK_EQ(raw->Bind(Addr(A, 5000)), Errno::BADF);
    CHECK_EQ(raw->Connect(Addr(B, 5000)), Errno::BADF);
    CHECK_EQ(raw->GetSockName().second, Errno::BADF);
    CHECK_EQ(raw->GetPeerName().second, Errno::BADF);
    CHECK_EQ(Send(*raw, Text("x"), Addr(B, 5000)).second, Errno::BADF);
    CHECK_EQ(raw->Send(Text("x"), 0).second, Errno::BADF);
    CHECK_EQ(Receive(*raw, 8).error, Errno::BADF);
    CHECK_EQ(raw->SetRcvTimeo(10), Errno::BADF);
    CHECK_EQ(raw->Close(), Errno::SUCCESS);
    CHECK_EQ(env.broker->Requests().size(), 0u);
    env.ExpectCleanBroker();
}

// Removes the directories a test made even when a check throws part-way through.
struct DirectoryCleanup {
    std::vector<std::string> paths;
    ~DirectoryCleanup() {
        std::error_code ignored;
        for (const auto& path : paths) {
            std::filesystem::remove_all(path, ignored);
        }
    }
};

void InitializeRejectsUnsafeConfiguration() {
    DirectoryCleanup cleanup;
    const auto make_dir = [&cleanup](mode_t mode) {
        std::string pattern = "/tmp/eln-cfg-XXXXXX";
        CHECK(::mkdtemp(pattern.data()) != nullptr);
        cleanup.paths.push_back(pattern);
        CHECK(::chmod(pattern.c_str(), mode) == 0);
        return pattern;
    };
    const auto init = [](const std::string& control, const std::string& datagram, Ip address) {
        NsoLanSocket socket{control, datagram, address};
        const Errno result = socket.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP);
        CHECK_EQ(socket.IsOpened(), result == Errno::SUCCESS);
        return result;
    };
    const auto leftover = [](const std::string& dir) {
        std::size_t count = 0;
        for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(dir)) {
            ++count;
        }
        return count;
    };

    const std::string good = make_dir(0700);
    CHECK_EQ(init(good + "/c.sock", good + "/d.sock", A), Errno::SUCCESS);
    CHECK_EQ(leftover(good), 0u); // destructor removed its socket file

    // Virtual address policy.
    for (const Ip& bad : {Ip{8, 8, 8, 8}, Any, Ip{10, 253, 0, 0}, Ip{10, 253, 255, 255}, ServiceOne,
                          Ip{127, 0, 0, 1}, Ip{10, 252, 0, 1}}) {
        CHECK_EQ(init(good + "/c.sock", good + "/d.sock", bad), Errno::INVAL);
    }
    CHECK_EQ(leftover(good), 0u);

    // Path policy.
    CHECK_EQ(init("c.sock", "d.sock", A), Errno::INVAL);                 // relative
    CHECK_EQ(init("", "", A), Errno::INVAL);                              // empty
    CHECK_EQ(init(good + "/", good + "/d.sock", A), Errno::INVAL);        // no file name
    const std::string other = make_dir(0700);
    CHECK_EQ(init(good + "/c.sock", other + "/d.sock", A), Errno::INVAL); // different directories
    CHECK_EQ(init(good + "/c.sock", good + "/sub/d.sock", A), Errno::INVAL);
    CHECK_EQ(init(good + "/" + std::string(120, 'x'), good + "/d.sock", A), Errno::INVAL); // too long
    CHECK_EQ(init(good + "/c.sock", good + "/" + std::string(120, 'y'), A), Errno::INVAL);
    CHECK_EQ(init(good + "/a\0b"s, good + "/d.sock", A), Errno::INVAL);
    CHECK_EQ(leftover(good), 0u);
    CHECK_EQ(leftover(other), 0u);

    // Directory policy: a missing directory, a symlink to a good one, and writable-by-others.
    CHECK_EQ(init(good + "/missing/c.sock", good + "/missing/d.sock", A), Errno::INVAL);
    const std::string link = good + "-link";
    CHECK(::symlink(good.c_str(), link.c_str()) == 0);
    cleanup.paths.push_back(link);
    CHECK_EQ(init(link + "/c.sock", link + "/d.sock", A), Errno::INVAL);
    CHECK(::chmod(good.c_str(), 0770) == 0);
    CHECK_EQ(init(good + "/c.sock", good + "/d.sock", A), Errno::INVAL); // group writable
    CHECK(::chmod(good.c_str(), 0707) == 0);
    CHECK_EQ(init(good + "/c.sock", good + "/d.sock", A), Errno::INVAL); // other writable
    CHECK(::chmod(good.c_str(), 01777) == 0);
    CHECK_EQ(init(good + "/c.sock", good + "/d.sock", A), Errno::INVAL); // sticky world-writable
    CHECK(::chmod(good.c_str(), 0750) == 0);
    CHECK_EQ(init(good + "/c.sock", good + "/d.sock", A), Errno::SUCCESS); // read-only for group is fine
    CHECK_EQ(leftover(good), 0u);

    // A regular file where the directory should be.
    const std::string file = good + "/not-a-directory";
    CHECK(::chmod(good.c_str(), 0700) == 0);
    { std::ofstream{file} << "x"; }
    CHECK_EQ(init(file + "/c.sock", file + "/d.sock", A), Errno::INVAL);
}

void CloseUnbindsAndCleansUp() {
    Env env{A, B};
    auto a = env.Open(0);
    CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::SUCCESS);
    const std::string path = SocketPathOf(env.broker->Requests().at(0));
    CHECK(env.broker->IsRegistered(A, 5000));
    CHECK_EQ(env.SocketFiles().size(), 1u);

    CHECK_EQ(a->Close(), Errno::SUCCESS);
    CHECK(!a->IsOpened());
    CHECK_EQ(a->GetFD(), SocketBase::INVALID_SOCKET);
    const auto requests = env.broker->Requests();
    CHECK_EQ(requests.size(), 2u);
    const auto unbind = nlohmann::json::parse(requests[1].line);
    CHECK_EQ(unbind.size(), 2u);
    CHECK_EQ(unbind.at("op").get<std::string>(), std::string{"unbind"});
    CHECK_EQ(unbind.at("socket").get<std::string>(), path);
    CHECK(!env.broker->IsRegistered(A, 5000)); // the broker finished unbinding before Close returned
    CHECK(env.SocketFiles().empty());
    struct stat info{};
    CHECK(::lstat(path.c_str(), &info) != 0);

    // Close is idempotent and sends nothing more; later calls fail cleanly.
    CHECK_EQ(a->Close(), Errno::SUCCESS);
    CHECK_EQ(env.broker->Requests().size(), 2u);
    CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::BADF);
    CHECK_EQ(Send(*a, Text("x"), Addr(B, 5000)).second, Errno::BADF);
    CHECK_EQ(Receive(*a, 8).error, Errno::BADF);
    CHECK_EQ(a->GetSockName().second, Errno::BADF);

    // The guest port is immediately reusable, by a new socket of the same instance.
    auto again = env.Open(0);
    CHECK_EQ(again->Bind(Addr(A, 5000)), Errno::SUCCESS);
    CHECK_EQ(env.SocketFiles().size(), 1u);

    // Destroying without Close does the same; a never-bound socket sends no unbind.
    const auto before = env.broker->Requests().size();
    again.reset();
    CHECK_EQ(env.broker->Requests().size(), before + 1);
    CHECK_EQ(nlohmann::json::parse(env.broker->Requests().back().line).at("op").get<std::string>(),
             std::string{"unbind"});
    CHECK(!env.broker->IsRegistered(A, 5000));
    CHECK(env.SocketFiles().empty());
    {
        auto idle = env.Open(0);
        CHECK_EQ(env.SocketFiles().size(), 1u);
    }
    CHECK_EQ(env.broker->Requests().size(), before + 1);
    CHECK(env.SocketFiles().empty());

    // Registrations of one socket do not affect another's unbind.
    auto first = env.Open(0);
    auto second = env.Open(0);
    CHECK_EQ(first->Bind(Addr(A, 5300)), Errno::SUCCESS);
    CHECK_EQ(second->Bind(Addr(A, 5301)), Errno::SUCCESS);
    first.reset();
    CHECK(!env.broker->IsRegistered(A, 5300));
    CHECK(env.broker->IsRegistered(A, 5301));
    env.ExpectCleanBroker();
}

void CleanupOnlyRemovesItsOwnNode() {
    Env env{A};
    // Case 1: the socket file was replaced by a regular file belonging to someone else.
    {
        auto a = env.Open(0);
        CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::SUCCESS);
        const std::string path = SocketPathOf(env.broker->Requests().back());
        CHECK(::unlink(path.c_str()) == 0);
        { std::ofstream{path} << "precious"; }
        TestSupport::ClearLog();
        CHECK_EQ(a->Close(), Errno::SUCCESS);
        std::ifstream in{path};
        std::string content;
        in >> content;
        CHECK_EQ(content, std::string{"precious"});
        CHECK(TestSupport::LogContains(Common::Log::Level::Warning, "leaving it alone"));
        CHECK(::unlink(path.c_str()) == 0);
    }
    // Case 2: replaced by a different socket node with the same name (another inode).
    {
        auto a = env.Open(0);
        const auto files = env.SocketFiles();
        CHECK_EQ(files.size(), 1u);
        const std::string path = env.dir + "/" + files[0];
        CHECK(::unlink(path.c_str()) == 0);
        const int impostor = ::socket(AF_UNIX, SOCK_DGRAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size());
        CHECK(::bind(impostor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        CHECK_EQ(a->Close(), Errno::SUCCESS);
        struct stat info{};
        CHECK(::lstat(path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode));
        ::close(impostor);
        CHECK(::unlink(path.c_str()) == 0);
    }
    // Case 3: nothing else in the directory is ever touched.
    {
        const std::string bystander = env.dir + "/bystander.sock";
        { std::ofstream{bystander} << "keep"; }
        { auto a = env.Open(0); }
        struct stat info{};
        CHECK(::lstat(bystander.c_str(), &info) == 0);
    }
    env.ExpectCleanBroker();
}

void SocketNamesAreUniqueAndNothingLeaks() {
    Env env{A};
    {
        std::vector<std::unique_ptr<NsoLanSocket>> sockets;
        for (int i = 0; i < 64; ++i) {
            sockets.push_back(env.Open(0));
        }
        const auto files = env.SocketFiles();
        CHECK_EQ(files.size(), 64u);
        CHECK_EQ(std::set<std::string>(files.begin(), files.end()).size(), 64u);
        for (const auto& name : files) {
            CHECK(name.size() <= 60);
            CHECK_EQ(name.substr(name.size() - 5), std::string{".sock"});
        }
    }
    CHECK(env.SocketFiles().empty());

    // Repeated open/bind/close and open/destroy cycles keep neither descriptors nor files.
    const std::size_t fds_before = OpenFdCount();
    for (int i = 0; i < 150; ++i) {
        auto a = env.Open(0);
        CHECK_EQ(a->Bind(Addr(A, 5000)), Errno::SUCCESS);
        CHECK_EQ(a->Close(), Errno::SUCCESS);
        auto b = env.Open(0);
        CHECK_EQ(b->Connect(Addr(B, 5000)), Errno::SUCCESS);
    }
    // The mock closes its end of the last control connection just after answering.
    for (int attempt = 0; attempt < 200 && OpenFdCount() != fds_before; ++attempt) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK_EQ(OpenFdCount(), fds_before);
    CHECK(env.SocketFiles().empty());
    CHECK(!env.broker->IsRegistered(A, 5000));
    env.ExpectCleanBroker();
}

void ShutdownAndUnsupportedOperations() {
    Env env{A, B};
    auto a = env.Open(0);
    auto b = env.Open(1);
    CHECK_EQ(b->Bind(Addr(B, 7000)), Errno::SUCCESS);
    b->SetRcvTimeo(2000);
    TestSupport::ClearLog();
    const auto asserts_before = TestSupport::SoftAssertCount();

    // Stream-style operations are explicit errors, not fake success and not assertions.
    CHECK_EQ(a->Listen(5), Errno::OTHER);
    CHECK_EQ(a->Accept().second, Errno::OTHER);
    CHECK(a->Accept().first.socket == nullptr);
    CHECK_EQ(TestSupport::SoftAssertCount(), asserts_before);

    CHECK_EQ(a->Connect(Addr(B, 7000)), Errno::SUCCESS);
    CHECK_EQ(a->Send(Text("before"), 0).second, Errno::SUCCESS);
    CHECK_EQ(a->Shutdown(ShutdownHow::WR), Errno::SUCCESS);
    CHECK_EQ(a->Send(Text("after"), 0), (std::pair<s32, Errno>{-1, Errno::PIPE}));
    CHECK_EQ(Send(*a, Text("after"), Addr(B, 7000)).second, Errno::PIPE);
    a->SetNonBlock(true);
    CHECK_EQ(Receive(*a, 8).error, Errno::AGAIN); // reads are still allowed
    CHECK_EQ(a->Shutdown(ShutdownHow::RD), Errno::SUCCESS);
    const auto eof = Receive(*a, 8);
    CHECK_EQ(eof.size, 0);
    CHECK_EQ(eof.error, Errno::SUCCESS);
    CHECK_EQ(Receive(*b, 16).data, Text("before"));
    CHECK_EQ(TestSupport::SoftAssertCount(), asserts_before);
    env.ExpectCleanBroker();
}

void ShutdownWakesBlockedReceiver() {
    for (int mode = 0; mode < 3; ++mode) {
        Env env{A, B};
        auto a = env.Open(0);
        CHECK_EQ(a->Bind(Addr(A, 7100)), Errno::SUCCESS);
        if (mode == 1) {
            CHECK_EQ(a->Connect(Addr(B, 7101)), Errno::SUCCESS);
        }
        // The timeout bounds a broken implementation; the success deadline is much shorter.
        CHECK_EQ(a->SetRcvTimeo(2000), Errno::SUCCESS);
        std::promise<std::pair<s32, Errno>> result;
        auto future = result.get_future();
        std::atomic<long> tid{0};
        std::jthread receiver([&] {
            tid.store(::syscall(SYS_gettid), std::memory_order_release);
            std::array<u8, 8> bytes{};
            result.set_value(a->Recv(0, bytes));
        });
        // Observe the real recvmsg syscall, rather than assume the receiver started after a sleep.
        bool blocked = false;
        const auto deadline = std::chrono::steady_clock::now() + 1000ms;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto id = tid.load(std::memory_order_acquire);
            if (id) {
                std::ifstream state{"/proc/self/task/" + std::to_string(id) + "/syscall"};
                long syscall_number = -1;
                if ((state >> syscall_number) && syscall_number == SYS_recvmsg) {
                    blocked = true;
                    break;
                }
            }
            std::this_thread::sleep_for(1ms);
        }
        CHECK(blocked);
        const auto error = mode == 2 ? a->Close() : a->Shutdown(ShutdownHow::RD);
        CHECK_EQ(error, mode == 0 ? Errno::NOTCONN : Errno::SUCCESS);
        const bool woke = future.wait_for(750ms) == std::future_status::ready;
        receiver.join();
        CHECK(woke);
        CHECK_EQ(future.get(), (std::pair<s32, Errno>{0, Errno::SUCCESS}));
        env.ExpectCleanBroker();
    }
}

struct Test {
    const char* name;
    void (*run)();
};

} // Anonymous namespace

int main(int argc, char** argv) {
    // Initializes the interrupt pipe that Network::Poll multiplexes with every socket.
    const NetworkInstance network;

    const std::vector<Test> tests{
        {"WireCodecMatchesLiteralBytes", WireCodecMatchesLiteralBytes},
        {"TwoInstancesSameGuestPortsRoundTrip", TwoInstancesSameGuestPortsRoundTrip},
        {"JsonRequestsEscapePaths", JsonRequestsEscapePaths},
        {"ServicesAndPayloadLimits", ServicesAndPayloadLimits},
        {"ZeroLengthTruncationAndPeek", ZeroLengthTruncationAndPeek},
        {"PollBlockingNonBlockingAndTimeouts", PollBlockingNonBlockingAndTimeouts},
        {"MalformedDatagramsAreErrorsNotPayload", MalformedDatagramsAreErrorsNotPayload},
        {"BindRulesAndBrokerFailures", BindRulesAndBrokerFailures},
        {"BrokerRefusalCodesMapToErrno", BrokerRefusalCodesMapToErrno},
        {"BrokerUnavailableOrSilent", BrokerUnavailableOrSilent},
        {"AutoBindAndConnectedSockets", AutoBindAndConnectedSockets},
        {"ImplicitBindIsRaceFree", ImplicitBindIsRaceFree},
        {"DestinationAndBroadcastRules", DestinationAndBroadcastRules},
        {"InitializeAcceptsOnlyUdpDatagrams", InitializeAcceptsOnlyUdpDatagrams},
        {"InitializeRejectsUnsafeConfiguration", InitializeRejectsUnsafeConfiguration},
        {"CloseUnbindsAndCleansUp", CloseUnbindsAndCleansUp},
        {"CleanupOnlyRemovesItsOwnNode", CleanupOnlyRemovesItsOwnNode},
        {"SocketNamesAreUniqueAndNothingLeaks", SocketNamesAreUniqueAndNothingLeaks},
        {"ShutdownAndUnsupportedOperations", ShutdownAndUnsupportedOperations},
        {"ShutdownWakesBlockedReceiver", ShutdownWakesBlockedReceiver},
    };

    const std::string filter = argc > 1 ? argv[1] : "";
    int passed = 0;
    int failed = 0;
    for (const auto& test : tests) {
        if (!filter.empty() && std::string{test.name}.find(filter) == std::string::npos) {
            continue;
        }
        const auto asserts_before = TestSupport::SoftAssertCount();
        Stopwatch watch;
        try {
            test.run();
            if (TestSupport::SoftAssertCount() != asserts_before) {
                throw TestFailure{"a production assert fired"};
            }
            std::printf("PASS %-42s %7.0f ms\n", test.name, watch.Ms());
            ++passed;
        } catch (const TestFailure& failure) {
            std::printf("FAIL %-42s %s\n", test.name, failure.message.c_str());
            ++failed;
        } catch (const std::exception& error) {
            std::printf("FAIL %-42s unexpected exception: %s\n", test.name, error.what());
            ++failed;
        }
        std::fflush(stdout);
    }
    std::printf("SUMMARY tests=%d passed=%d failed=%d soft_asserts=%zu\n", passed + failed, passed,
                failed, TestSupport::SoftAssertCount());
    return failed == 0 && passed > 0 ? 0 : 1;
}
