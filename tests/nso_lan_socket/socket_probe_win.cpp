// SPDX-License-Identifier: GPL-3.0-or-later
// Regression probe for the Windows Network::NsoLanSocket (nso_lan_socket_win.cpp). It runs the
// production class and the unmodified base Socket/Poll implementation (WSAPoll) against a mock
// broker that speaks the stream form of the wire protocol over real AF_UNIX sockets
// (mock_broker_win.h). Expected bytes are literals written from the wire description.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include "core/internal_network/network.h"
#include "core/internal_network/nso_lan_platform.h"
#include "core/internal_network/nso_lan_socket.h"
#include "core/internal_network/sockets.h"
#include "mock_broker_win.h"
#include "test_support.h"

using namespace Network;
using namespace std::chrono_literals;
using Mock::Bytes;
using Mock::Ip;

namespace {

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
std::string Show(PollEvents events) {
    return std::to_string(static_cast<u16>(events));
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

// ---------------------------------------------------------------------------------------------
// Fixtures and helpers
// ---------------------------------------------------------------------------------------------

constexpr Ip AddrA{10, 253, 1, 1};
constexpr Ip AddrB{10, 253, 1, 2};

SockAddrIn Endpoint(Ip ip, u16 port) {
    return SockAddrIn{.family = Domain::INET, .ip = ip, .portno = port};
}

std::string NewPrivateDirectory() {
    static int counter = 0;
    char* local = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&local, &length, "LOCALAPPDATA") != 0 || local == nullptr) {
        Fail(__FILE__, __LINE__, "LOCALAPPDATA is not set");
    }
    std::string base{local};
    std::free(local);
    const std::string directory =
        base + "\\Temp\\eln-probe-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(counter++);
    if (!NsoLan::EnsurePrivateDirectory(directory)) {
        Fail(__FILE__, __LINE__, "could not create a private directory at " + directory);
    }
    return directory;
}

void RemoveDirectoryTree(const std::string& directory) {
    std::error_code ignored;
    std::filesystem::remove_all(std::filesystem::path{directory}, ignored);
}

std::vector<std::string> NodesIn(const std::string& directory) {
    std::vector<std::string> out;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator{std::filesystem::path{directory}, ignored}) {
        const auto name = entry.path().filename().string();
        if (name.rfind("eln-", 0) == 0) {
            out.push_back(entry.path().string());
        }
    }
    return out;
}

bool IsReparsePoint(const std::string& path) {
    const DWORD attributes = ::GetFileAttributesW(std::filesystem::path{path}.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

struct Fixture {
    std::string directory;
    Mock::Broker broker;
    std::size_t a;
    std::size_t b;

    Fixture() : directory{NewPrivateDirectory()}, broker{directory} {
        a = broker.AddInstance(AddrA);
        b = broker.AddInstance(AddrB);
        if (!broker.Start()) {
            Fail(__FILE__, __LINE__, "mock broker did not start");
        }
    }
    ~Fixture() {
        broker.Stop();
        RemoveDirectoryTree(directory);
    }

    std::unique_ptr<NsoLanSocket> Open(std::size_t instance) {
        auto socket = std::make_unique<NsoLanSocket>(broker.ControlPath(instance), broker.DatagramPath(instance),
                                                     broker.Address(instance));
        CHECK_EQ(socket->Initialize(Domain::INET, Type::DGRAM, Protocol::UDP), Errno::SUCCESS);
        return socket;
    }

    /// The node the most recent hello named.
    std::string LastNode() {
        const auto hellos = broker.Hellos();
        CHECK(!hellos.empty());
        const auto body = nlohmann::json::parse(hellos.back().line, nullptr, false);
        CHECK(body.is_object() && body.value("hello", "") == "client" && body["socket"].is_string());
        return body["socket"].get<std::string>();
    }

    void NoViolations() {
        const auto violations = broker.Violations();
        if (!violations.empty()) {
            Fail(__FILE__, __LINE__, "mock saw a protocol violation: " + violations.front());
        }
    }
};

PollEvents PollOnce(SocketBase& socket, int timeout_ms) {
    std::vector<PollFD> fds{PollFD{.socket = &socket, .events = PollEvents::In, .revents = PollEvents{}}};
    const auto [count, error] = Network::Poll(fds, timeout_ms);
    CHECK_EQ(error, Errno::SUCCESS);
    return fds[0].revents;
}

bool Readable(PollEvents events) {
    return (events & (PollEvents::In | PollEvents::RdNorm)) != PollEvents{};
}

Bytes Payload(std::string_view text) {
    return Bytes(text.begin(), text.end());
}

std::pair<s32, Errno> RecvInto(NsoLanSocket& socket, Bytes& buffer, int flags = 0, SockAddrIn* addr = nullptr) {
    return socket.RecvFrom(flags, std::span<u8>{buffer.data(), buffer.size()}, addr);
}

// ---------------------------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------------------------

void PrivateDirectoryCreationNeverAdoptsExistingPaths() {
    Fixture f;
    const auto first = f.directory + "\\instance-first";
    const auto second = f.directory + "\\instance-second";
    CHECK(NsoLan::CreatePrivateDirectory(first));
    CHECK(NsoLan::CreatePrivateDirectory(second));
    CHECK(NsoLan::IsPrivateDirectory(first));
    CHECK(NsoLan::IsPrivateDirectory(second));
    // Existing private directories remain usable, but cannot be claimed as newly owned.
    CHECK(!NsoLan::CreatePrivateDirectory(first));
    CHECK(NsoLan::EnsurePrivateDirectory(first));
    const auto marker = first + "\\keep.txt";
    {
        std::ofstream file{marker};
        file << "preserve-existing-content";
    }
    CHECK(!NsoLan::CreatePrivateDirectory(marker));
    CHECK(!NsoLan::CreatePrivateDirectory(first));
    std::ifstream file{marker};
    std::string content;
    std::getline(file, content);
    CHECK(content == "preserve-existing-content");
    CHECK(!NsoLan::CreatePrivateDirectory(f.directory + "\\missing-parent\\child"));
    CHECK(!std::filesystem::exists(f.directory + "\\missing-parent"));
}

void InitializeRejectsWrongTypesAndNonPrivateDirectories() {
    Fixture f;
    NsoLanSocket stream{f.broker.ControlPath(f.a), f.broker.DatagramPath(f.a), AddrA};
    CHECK_EQ(stream.Initialize(Domain::INET, Type::STREAM, Protocol::TCP), Errno::INVAL);
    NsoLanSocket outside{f.broker.ControlPath(f.a), f.broker.DatagramPath(f.a), Ip{192, 168, 1, 1}};
    CHECK_EQ(outside.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP), Errno::INVAL);
    // A directory with the ordinary inherited profile ACL is not private.
    char* temp = nullptr;
    std::size_t length = 0;
    CHECK(_dupenv_s(&temp, &length, "TEMP") == 0 && temp != nullptr);
    const std::string plain = std::string{temp} + "\\eln-plain-" + std::to_string(::GetCurrentProcessId());
    std::free(temp);
    CHECK(::CreateDirectoryA(plain.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS);
    CHECK(!NsoLan::IsPrivateDirectory(plain));
    NsoLanSocket in_plain{plain + "\\control.sock", plain + "\\data.sock", AddrA};
    CHECK_EQ(in_plain.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP), Errno::INVAL);
    RemoveDirectoryTree(plain);
    CHECK(NsoLan::IsPrivateDirectory(f.directory));
    CHECK(f.broker.Hellos().empty());
}

void InitializeCreatesNodeSaysHelloAndIsNotReadable() {
    Fixture f;
    auto s = f.Open(f.a);
    CHECK(f.broker.WaitForHellos(1, 2000ms));
    const std::string node = f.LastNode();
    CHECK(node.rfind(f.directory + "\\eln-", 0) == 0);
    CHECK(IsReparsePoint(node));
    CHECK(f.broker.HasStream(node));
    CHECK(s->IsOpened());
    CHECK_EQ(PollOnce(*s, 100), PollEvents{});
    CHECK_EQ(s->Close(), Errno::SUCCESS);
    CHECK(!IsReparsePoint(node));
    f.NoViolations();
}

void BindRegistersAndRoutesLiteralBytes() {
    Fixture f;
    auto sa = f.Open(f.a);
    auto sb = f.Open(f.b);
    CHECK(f.broker.WaitForHellos(2, 2000ms));
    CHECK_EQ(sa->Bind(Endpoint({}, 50500)), Errno::SUCCESS);
    CHECK_EQ(sb->Bind(Endpoint(AddrB, 50600)), Errno::SUCCESS);
    CHECK(f.broker.WaitForRequests(2, 2000ms));
    const auto requests = f.broker.Requests();
    const auto hellos = f.broker.Hellos();
    const std::string node_a = nlohmann::json::parse(hellos[0].line)["socket"].get<std::string>();
    CHECK_EQ(requests[0].line, "{\"op\":\"bind\",\"port\":50500,\"socket\":\"" + nlohmann::json(node_a).dump().substr(1, nlohmann::json(node_a).dump().size() - 2) + "\"}");
    CHECK(f.broker.IsRegistered(AddrA, 50500));
    CHECK(f.broker.IsRegistered(AddrB, 50600));
    const auto [name, name_error] = sa->GetSockName();
    CHECK_EQ(name_error, Errno::SUCCESS);
    CHECK(name.ip == AddrA && name.portno == 50500);

    const Bytes payload = Payload("hello");
    const SockAddrIn to_b = Endpoint(AddrB, 50600);
    CHECK_EQ(sa->SendTo(0, std::span<const u8>{payload.data(), payload.size()}, &to_b), (std::pair<s32, Errno>{5, Errno::SUCCESS}));
    CHECK(f.broker.WaitForFramesHandled(1, 2000ms));
    const auto frames = f.broker.Frames();
    CHECK_EQ(frames[0].bytes, Mock::OracleEncode(AddrA, AddrB, 50500, 50600, payload));
    CHECK(Readable(PollOnce(*sb, 1000)));
    Bytes buffer(64);
    SockAddrIn from{};
    CHECK_EQ(RecvInto(*sb, buffer, 0, &from), (std::pair<s32, Errno>{5, Errno::SUCCESS}));
    CHECK_EQ(Bytes(buffer.begin(), buffer.begin() + 5), payload);
    CHECK(from.family == Domain::INET && from.ip == AddrA && from.portno == 50500);
    CHECK_EQ(PollOnce(*sb, 50), PollEvents{});
    f.NoViolations();
}

void ZeroLengthAndMaximumPayloads() {
    Fixture f;
    auto sa = f.Open(f.a);
    auto sb = f.Open(f.b);
    CHECK_EQ(sa->Bind(Endpoint({}, 50500)), Errno::SUCCESS);
    CHECK_EQ(sb->Bind(Endpoint({}, 50600)), Errno::SUCCESS);
    const SockAddrIn to_b = Endpoint(AddrB, 50600);
    CHECK_EQ(sa->SendTo(0, std::span<const u8>{}, &to_b), (std::pair<s32, Errno>{0, Errno::SUCCESS}));
    CHECK(Readable(PollOnce(*sb, 1000)));
    Bytes buffer(8, 0xaa);
    CHECK_EQ(RecvInto(*sb, buffer), (std::pair<s32, Errno>{0, Errno::SUCCESS}));
    CHECK_EQ(PollOnce(*sb, 50), PollEvents{});

    Bytes big(NsoLanWire::MaxPayloadSize);
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<u8>(i * 7);
    }
    CHECK_EQ(sa->SendTo(0, std::span<const u8>{big.data(), big.size()}, &to_b),
             (std::pair<s32, Errno>{static_cast<s32>(big.size()), Errno::SUCCESS}));
    CHECK(Readable(PollOnce(*sb, 2000)));
    Bytes large(NsoLanWire::MaxPayloadSize + 16);
    CHECK_EQ(RecvInto(*sb, large), (std::pair<s32, Errno>{static_cast<s32>(big.size()), Errno::SUCCESS}));
    CHECK(std::equal(big.begin(), big.end(), large.begin()));
    Bytes too_big(NsoLanWire::MaxPayloadSize + 1);
    CHECK_EQ(sa->SendTo(0, std::span<const u8>{too_big.data(), too_big.size()}, &to_b),
             (std::pair<s32, Errno>{-1, Errno::MSGSIZE}));
    f.NoViolations();
}

void TruncationPeekAndNonBlocking() {
    Fixture f;
    auto sa = f.Open(f.a);
    auto sb = f.Open(f.b);
    CHECK_EQ(sa->Bind(Endpoint({}, 50500)), Errno::SUCCESS);
    CHECK_EQ(sb->Bind(Endpoint({}, 50600)), Errno::SUCCESS);
    const SockAddrIn to_b = Endpoint(AddrB, 50600);
    Bytes hundred(100);
    for (std::size_t i = 0; i < hundred.size(); ++i) {
        hundred[i] = static_cast<u8>(i);
    }
    CHECK_EQ(sa->SendTo(0, std::span<const u8>{hundred.data(), hundred.size()}, &to_b),
             (std::pair<s32, Errno>{100, Errno::SUCCESS}));
    CHECK(Readable(PollOnce(*sb, 1000)));
    Bytes ten(10);
    CHECK_EQ(RecvInto(*sb, ten, FLAG_MSG_PEEK), (std::pair<s32, Errno>{10, Errno::SUCCESS}));
    CHECK(std::equal(ten.begin(), ten.end(), hundred.begin()));
    CHECK(Readable(PollOnce(*sb, 50))); // still queued after a peek
    CHECK_EQ(RecvInto(*sb, ten), (std::pair<s32, Errno>{10, Errno::SUCCESS})); // head only, rest discarded
    CHECK_EQ(PollOnce(*sb, 50), PollEvents{});
    // Non-blocking mode and MSG_DONTWAIT on an empty queue.
    CHECK_EQ(sb->SetNonBlock(true), Errno::SUCCESS);
    CHECK_EQ(RecvInto(*sb, ten), (std::pair<s32, Errno>{-1, Errno::AGAIN}));
    CHECK_EQ(sb->SetNonBlock(false), Errno::SUCCESS);
    CHECK_EQ(RecvInto(*sb, ten, FLAG_MSG_DONTWAIT), (std::pair<s32, Errno>{-1, Errno::AGAIN}));
    CHECK_EQ(RecvInto(*sb, ten, 0x40), (std::pair<s32, Errno>{-1, Errno::INVAL})); // unsupported flag
    f.NoViolations();
}

void ConnectedSocketFiltersOtherSources() {
    Fixture f;
    auto sa = f.Open(f.a);
    auto sb = f.Open(f.b);
    CHECK(f.broker.WaitForHellos(2, 2000ms));
    CHECK_EQ(sa->Bind(Endpoint({}, 50500)), Errno::SUCCESS);
    CHECK_EQ(sb->Bind(Endpoint({}, 50600)), Errno::SUCCESS);
    CHECK_EQ(sb->Connect(Endpoint(AddrA, 50500)), Errno::SUCCESS);
    const auto [peer, peer_error] = sb->GetPeerName();
    CHECK_EQ(peer_error, Errno::SUCCESS);
    CHECK(peer.ip == AddrA && peer.portno == 50500);
    const std::string node_b = nlohmann::json::parse(f.broker.Hellos()[1].line)["socket"].get<std::string>();
    // A record from another virtual endpoint is dropped silently; one from the peer is delivered.
    CHECK_EQ(f.broker.Inject(node_b, Mock::OracleEncode(Ip{10, 253, 1, 3}, AddrB, 7, 50600, Payload("stranger"))), 0);
    CHECK_EQ(f.broker.Inject(node_b, Mock::OracleEncode(AddrA, AddrB, 50500, 50600, Payload("peer"))), 0);
    CHECK(Readable(PollOnce(*sb, 1000)));
    Bytes buffer(16);
    CHECK_EQ(sb->Recv(0, std::span<u8>{buffer.data(), buffer.size()}), (std::pair<s32, Errno>{4, Errno::SUCCESS}));
    CHECK_EQ(Bytes(buffer.begin(), buffer.begin() + 4), Payload("peer"));
    CHECK_EQ(PollOnce(*sb, 50), PollEvents{});
    // Send on the connected socket reaches the peer with the connected destination.
    const Bytes reply = Payload("back");
    CHECK_EQ(sb->Send(std::span<const u8>{reply.data(), reply.size()}, 0), (std::pair<s32, Errno>{4, Errno::SUCCESS}));
    CHECK(Readable(PollOnce(*sa, 1000)));
    SockAddrIn from{};
    CHECK_EQ(RecvInto(*sa, buffer, 0, &from), (std::pair<s32, Errno>{4, Errno::SUCCESS}));
    CHECK(from.ip == AddrB && from.portno == 50600);
    f.NoViolations();
}

void RecordsNotForThisSocketAreRejected() {
    Fixture f;
    auto sb = f.Open(f.b);
    CHECK(f.broker.WaitForHellos(1, 2000ms));
    CHECK_EQ(sb->Bind(Endpoint({}, 50600)), Errno::SUCCESS);
    const std::string node_b = f.LastNode();
    // Wrong destination port: rejected with INVAL and consumed (not with MSG_PEEK).
    CHECK_EQ(f.broker.Inject(node_b, Mock::OracleEncode(AddrA, AddrB, 50500, 50601, Payload("x"))), 0);
    CHECK(Readable(PollOnce(*sb, 1000)));
    Bytes buffer(16);
    CHECK_EQ(RecvInto(*sb, buffer, FLAG_MSG_PEEK), (std::pair<s32, Errno>{-1, Errno::INVAL}));
    CHECK(Readable(PollOnce(*sb, 50)));
    CHECK_EQ(RecvInto(*sb, buffer), (std::pair<s32, Errno>{-1, Errno::INVAL}));
    CHECK_EQ(PollOnce(*sb, 50), PollEvents{});
    TestSupport::ClearLog();
    // A source outside the virtual network is rejected too.
    CHECK_EQ(f.broker.Inject(node_b, Mock::OracleEncode(Ip{192, 168, 0, 9}, AddrB, 1, 50600, Payload("x"))), 0);
    CHECK(Readable(PollOnce(*sb, 1000)));
    CHECK_EQ(RecvInto(*sb, buffer), (std::pair<s32, Errno>{-1, Errno::INVAL}));
    CHECK(TestSupport::LogContains(Common::Log::Level::Warning, "Rejected LAN datagram"));
}

void CorruptStreamFromBrokerIsNetworkDown() {
    Fixture f;
    auto sb = f.Open(f.b);
    CHECK(f.broker.WaitForHellos(1, 2000ms));
    CHECK_EQ(sb->Bind(Endpoint({}, 50600)), Errno::SUCCESS);
    const std::string node_b = f.LastNode();
    Bytes bad = Mock::OracleEncode(AddrA, AddrB, 50500, 50600, Payload("x"));
    bad[0] = 'X';
    CHECK_EQ(f.broker.Inject(node_b, bad), 0);
    CHECK(Readable(PollOnce(*sb, 2000))); // EOF is signalled so a poller wakes
    Bytes buffer(16);
    CHECK_EQ(RecvInto(*sb, buffer), (std::pair<s32, Errno>{-1, Errno::NETDOWN}));
    CHECK_EQ(RecvInto(*sb, buffer), (std::pair<s32, Errno>{-1, Errno::NETDOWN}));
}

void BrokerClosingTheStreamIsNetworkDown() {
    Fixture f;
    auto sb = f.Open(f.b);
    CHECK(f.broker.WaitForHellos(1, 2000ms));
    CHECK_EQ(sb->Bind(Endpoint({}, 50600)), Errno::SUCCESS);
    CHECK(f.broker.CloseStream(f.LastNode()));
    CHECK(Readable(PollOnce(*sb, 2000)));
    Bytes buffer(16);
    CHECK_EQ(RecvInto(*sb, buffer), (std::pair<s32, Errno>{-1, Errno::NETDOWN}));
    // Sends on a closed stream fail (the first may still be accepted by the kernel buffer).
    const Bytes payload = Payload("late");
    const SockAddrIn to_a = Endpoint(AddrA, 50500);
    bool failed = false;
    for (int i = 0; i < 3 && !failed; ++i) {
        failed = sb->SendTo(0, std::span<const u8>{payload.data(), payload.size()}, &to_a).second != Errno::SUCCESS;
        std::this_thread::sleep_for(50ms);
    }
    CHECK(failed);
}

void ShutdownWakesBlockedReceiverAndPoll() {
    Fixture f;
    auto sb = f.Open(f.b);
    CHECK_EQ(sb->Bind(Endpoint({}, 50600)), Errno::SUCCESS);
    Bytes buffer(16);
    auto blocked = std::async(std::launch::async, [&] { return RecvInto(*sb, buffer); });
    CHECK(blocked.wait_for(300ms) == std::future_status::timeout); // really blocked
    CHECK_EQ(sb->Shutdown(ShutdownHow::RD), Errno::NOTCONN); // unconnected, like native UDP
    CHECK(blocked.wait_for(2000ms) == std::future_status::ready);
    CHECK_EQ(blocked.get(), (std::pair<s32, Errno>{0, Errno::SUCCESS}));
    CHECK(Readable(PollOnce(*sb, 100))); // readable EOF, like native UDP after shutdown
    CHECK_EQ(RecvInto(*sb, buffer), (std::pair<s32, Errno>{0, Errno::SUCCESS}));
    // Shutdown of the write side makes sends fail with PIPE.
    CHECK_EQ(sb->Shutdown(ShutdownHow::WR), Errno::NOTCONN);
    const Bytes payload = Payload("x");
    const SockAddrIn to_a = Endpoint(AddrA, 50500);
    CHECK_EQ(sb->SendTo(0, std::span<const u8>{payload.data(), payload.size()}, &to_a), (std::pair<s32, Errno>{-1, Errno::PIPE}));
}

void CloseWakesBlockedReceiverUnbindsAndRemovesNode() {
    Fixture f;
    auto sb = f.Open(f.b);
    CHECK(f.broker.WaitForHellos(1, 2000ms));
    const std::string node = f.LastNode();
    CHECK_EQ(sb->Bind(Endpoint({}, 50600)), Errno::SUCCESS);
    CHECK(f.broker.WaitForRequests(1, 2000ms));
    Bytes buffer(16);
    auto blocked = std::async(std::launch::async, [&] { return RecvInto(*sb, buffer); });
    CHECK(blocked.wait_for(300ms) == std::future_status::timeout);
    CHECK_EQ(sb->Close(), Errno::SUCCESS);
    CHECK(blocked.wait_for(2000ms) == std::future_status::ready);
    CHECK_EQ(blocked.get().second, Errno::BADF);
    CHECK(f.broker.WaitForRequests(2, 2000ms));
    const auto requests = f.broker.Requests();
    CHECK(requests[1].line.rfind("{\"op\":\"unbind\"", 0) == 0);
    CHECK(!f.broker.IsRegistered(AddrB, 50600));
    CHECK(!IsReparsePoint(node));
    CHECK(NodesIn(f.directory).empty());
    CHECK(!sb->IsOpened());
    CHECK_EQ(RecvInto(*sb, buffer), (std::pair<s32, Errno>{-1, Errno::BADF}));
    f.NoViolations();
}

void ReceiveTimeoutReturnsAgain() {
    Fixture f;
    auto sb = f.Open(f.b);
    CHECK_EQ(sb->Bind(Endpoint({}, 50600)), Errno::SUCCESS);
    CHECK_EQ(sb->SetRcvTimeo(200), Errno::SUCCESS);
    Bytes buffer(16);
    const auto start = std::chrono::steady_clock::now();
    CHECK_EQ(RecvInto(*sb, buffer), (std::pair<s32, Errno>{-1, Errno::AGAIN}));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed >= 150ms && elapsed < 2000ms);
    CHECK_EQ(sb->SetSndTimeo(500), Errno::SUCCESS);
}

void ControlRefusalsTimeoutsAndBindRules() {
    Fixture f;
    auto sa = f.Open(f.a);
    auto sb = f.Open(f.b);
    CHECK_EQ(sa->Bind(Endpoint(Ip{10, 253, 9, 9}, 50500)), Errno::ADDRNOTAVAIL); // not our address
    f.broker.ScriptReply(f.a, "{\"ok\":false,\"error\":\"port_in_use\"}\n");
    CHECK_EQ(sa->Bind(Endpoint({}, 50500)), Errno::ADDRINUSE);
    f.broker.ScriptReply(f.a, "{\"ok\":false,\"error\":\"forbidden\"}\n");
    CHECK_EQ(sa->Bind(Endpoint({}, 50500)), Errno::ACCES);
    f.broker.ScriptReply(f.a, "{\"ok\":false,\"error\":\"not_ready\"}\n");
    CHECK_EQ(sa->Bind(Endpoint({}, 50500)), Errno::NETDOWN);
    f.broker.ScriptReply(f.a, "nonsense\n");
    CHECK_EQ(sa->Bind(Endpoint({}, 50500)), Errno::OTHER);
    f.broker.ScriptReply(f.a, "{\"ok\":true,\"port\":50501}\n"); // a different port than requested
    CHECK_EQ(sa->Bind(Endpoint({}, 50500)), Errno::OTHER);
    f.broker.ScriptHang(f.a);
    const auto start = std::chrono::steady_clock::now();
    CHECK_EQ(sa->Bind(Endpoint({}, 50500)), Errno::TIMEDOUT);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed >= 1500ms && elapsed < 4000ms);
    // Real registry: the same port twice on one instance, and rebinding.
    CHECK_EQ(sa->Bind(Endpoint({}, 50500)), Errno::SUCCESS);
    auto sa2 = f.Open(f.a);
    CHECK_EQ(sa2->Bind(Endpoint({}, 50500)), Errno::ADDRINUSE);
    CHECK_EQ(sa->Bind(Endpoint({}, 50510)), Errno::INVAL);
    CHECK_EQ(sb->Bind(Endpoint({}, 50500)), Errno::SUCCESS); // same guest port on another instance
    CHECK_EQ(sa->Listen(1), Errno::OTHER);
    CHECK_EQ(sa->Accept().second, Errno::OTHER);
}

void HelloRefusedLeavesNoNode() {
    Fixture f;
    f.broker.ScriptHelloRefusal(f.a, "forbidden");
    NsoLanSocket refused{f.broker.ControlPath(f.a), f.broker.DatagramPath(f.a), AddrA};
    CHECK_EQ(refused.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP), Errno::ACCES);
    CHECK(NodesIn(f.directory).empty());
    CHECK(!refused.IsOpened());
    // A broker that is not there at all: the virtual network is down.
    NsoLanSocket absent{f.directory + "\\missing-control.sock", f.directory + "\\missing-data.sock", AddrA};
    CHECK_EQ(absent.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP), Errno::NETDOWN);
    CHECK(NodesIn(f.directory).empty());
}

void DestinationValidation() {
    Fixture f;
    auto sa = f.Open(f.a);
    CHECK_EQ(sa->Bind(Endpoint({}, 50500)), Errno::SUCCESS);
    const Bytes payload = Payload("x");
    const std::span<const u8> bytes{payload.data(), payload.size()};
    const SockAddrIn broadcast = Endpoint(Ip{255, 255, 255, 255}, 5);
    const SockAddrIn multicast = Endpoint(Ip{239, 1, 2, 3}, 5);
    const SockAddrIn outside = Endpoint(Ip{192, 168, 1, 1}, 5);
    const SockAddrIn zero_port = Endpoint(AddrB, 0);
    const SockAddrIn service = Endpoint(Ip{10, 254, 0, 1}, 10025);
    CHECK_EQ(sa->SendTo(0, bytes, &broadcast), (std::pair<s32, Errno>{-1, Errno::NETUNREACH}));
    CHECK_EQ(sa->SendTo(0, bytes, &multicast), (std::pair<s32, Errno>{-1, Errno::NETUNREACH}));
    CHECK_EQ(sa->SendTo(0, bytes, &outside), (std::pair<s32, Errno>{-1, Errno::NETUNREACH}));
    CHECK_EQ(sa->SendTo(0, bytes, &zero_port), (std::pair<s32, Errno>{-1, Errno::INVAL}));
    CHECK_EQ(sa->SendTo(0, bytes, &service), (std::pair<s32, Errno>{1, Errno::SUCCESS})); // virtual service is allowed
    CHECK(f.broker.WaitForFramesHandled(1, 2000ms));
    CHECK_EQ(sa->Send(bytes, 0), (std::pair<s32, Errno>{-1, Errno::NOTCONN}));
    CHECK_EQ(sa->SendTo(0x40, bytes, &service), (std::pair<s32, Errno>{-1, Errno::INVAL})); // unsupported flag
    f.NoViolations();
}

void QueueIsBounded() {
    Fixture f;
    auto sb = f.Open(f.b);
    CHECK(f.broker.WaitForHellos(1, 2000ms));
    CHECK_EQ(sb->Bind(Endpoint({}, 50600)), Errno::SUCCESS);
    const std::string node_b = f.LastNode();
    Bytes burst;
    for (int i = 0; i < 600; ++i) {
        const Bytes one = Mock::OracleEncode(AddrA, AddrB, 50500, 50600, Payload("burst"));
        burst.insert(burst.end(), one.begin(), one.end());
    }
    CHECK_EQ(f.broker.Inject(node_b, burst), 0);
    std::this_thread::sleep_for(500ms);
    int received = 0;
    Bytes buffer(16);
    while (RecvInto(*sb, buffer, FLAG_MSG_DONTWAIT).second == Errno::SUCCESS) {
        ++received;
    }
    CHECK_EQ(received, 512); // the newest frames beyond the bound were dropped, like a full receive buffer
    CHECK_EQ(PollOnce(*sb, 50), PollEvents{});
}

void ImplicitBindOnSendAndSameGuestPortAcrossInstances() {
    Fixture f;
    auto sa = f.Open(f.a);
    auto sb = f.Open(f.b);
    CHECK_EQ(sb->Bind(Endpoint({}, 50600)), Errno::SUCCESS);
    const Bytes payload = Payload("auto");
    const SockAddrIn to_b = Endpoint(AddrB, 50600);
    CHECK_EQ(sa->SendTo(0, std::span<const u8>{payload.data(), payload.size()}, &to_b), (std::pair<s32, Errno>{4, Errno::SUCCESS}));
    const auto [name, error] = sa->GetSockName();
    CHECK_EQ(error, Errno::SUCCESS);
    CHECK(name.portno >= 49152); // ephemeral registration happened implicitly
    CHECK(Readable(PollOnce(*sb, 1000)));
    Bytes buffer(16);
    SockAddrIn from{};
    CHECK_EQ(RecvInto(*sb, buffer, 0, &from), (std::pair<s32, Errno>{4, Errno::SUCCESS}));
    CHECK(from.ip == AddrA && from.portno == name.portno);
    f.NoViolations();
}

// --- W1 regressions: Close racing blocked or in-flight guest calls (observed blocked first) ---

void CloseWhileBlockedInRecvRepeated() {
    Fixture f;
    for (int i = 0; i < 20; ++i) {
        auto sb = f.Open(f.b);
        CHECK_EQ(sb->Bind(Endpoint({}, static_cast<u16>(51000 + i))), Errno::SUCCESS);
        Bytes buffer(16);
        auto blocked = std::async(std::launch::async, [&] { return RecvInto(*sb, buffer); });
        CHECK(blocked.wait_for(100ms) == std::future_status::timeout); // really blocked
        CHECK_EQ(sb->Close(), Errno::SUCCESS);
        CHECK(blocked.wait_for(2000ms) == std::future_status::ready);
        CHECK_EQ(blocked.get().second, Errno::BADF);
        CHECK_EQ(RecvInto(*sb, buffer), (std::pair<s32, Errno>{-1, Errno::BADF}));
    }
    CHECK(NodesIn(f.directory).empty());
    f.NoViolations();
}

void CloseRacingInFlightSendRepeated() {
    Fixture f;
    for (int i = 0; i < 20; ++i) {
        auto sa = f.Open(f.a);
        CHECK_EQ(sa->Bind(Endpoint({}, static_cast<u16>(52000 + i))), Errno::SUCCESS);
        const Bytes payload(256, static_cast<u8>(i));
        const SockAddrIn to_b = Endpoint(AddrB, 50600);
        std::atomic<int> sends{0};
        auto sender = std::async(std::launch::async, [&] {
            Errno last = Errno::SUCCESS;
            for (;;) {
                const auto result = sa->SendTo(0, std::span<const u8>{payload.data(), payload.size()}, &to_b);
                if (result.second != Errno::SUCCESS) {
                    last = result.second;
                    break;
                }
                ++sends;
            }
            return last;
        });
        while (sends.load() < 5) {
            std::this_thread::sleep_for(1ms); // observed in flight
        }
        CHECK_EQ(sa->Close(), Errno::SUCCESS);
        CHECK(sender.wait_for(5000ms) == std::future_status::ready);
        const Errno last = sender.get();
        CHECK(last == Errno::BADF || last == Errno::NETDOWN || last == Errno::PIPE || last == Errno::AGAIN);
    }
    f.NoViolations(); // every record the mock received was complete and well-formed
}

void CloseWhileBlockedInPollRepeated() {
    Fixture f;
    for (int i = 0; i < 20; ++i) {
        auto sb = f.Open(f.b);
        CHECK_EQ(sb->Bind(Endpoint({}, static_cast<u16>(53000 + i))), Errno::SUCCESS);
        auto polled = std::async(std::launch::async, [&] { return PollOnce(*sb, 5000); });
        CHECK(polled.wait_for(100ms) == std::future_status::timeout); // really blocked in WSAPoll
        CHECK_EQ(sb->Close(), Errno::SUCCESS);
        CHECK(polled.wait_for(2000ms) == std::future_status::ready);
        const PollEvents events = polled.get();
        CHECK(Readable(events));
        CHECK((events & PollEvents::Nval) == PollEvents{});
    }
    f.NoViolations();
}

void CloseInterruptsBackpressuredSend() {
    for (int iteration = 0; iteration < 10; ++iteration) {
        Fixture f;
        auto s = f.Open(f.a);
        CHECK_EQ(s->Bind(Endpoint({}, 53800)), Errno::SUCCESS);
        CHECK_EQ(s->SetSndTimeo(0), Errno::SUCCESS);
        f.broker.PauseDataReads(true);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!f.broker.DataReadsPaused() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        CHECK(f.broker.DataReadsPaused());
        const Bytes payload(NsoLanWire::MaxPayloadSize, 0x5a);
        const auto destination = Endpoint(AddrB, 50600);
        std::atomic<int> started{0}, completed{0};
        auto sender = std::async(std::launch::async, [&] {
            for (;;) {
                ++started;
                const auto result = s->SendTo(0, payload, &destination);
                if (result.second != Errno::SUCCESS) {
                    return result.second;
                }
                ++completed;
            }
        });
        // The broker has acknowledged that it no longer reads data. Wait for actual completed
        // writes, then observe a call in progress for 150ms with no write progress.
        const auto fill_deadline = std::chrono::steady_clock::now() + 3s;
        while (completed.load() == 0 && std::chrono::steady_clock::now() < fill_deadline) {
            std::this_thread::sleep_for(1ms);
        }
        std::this_thread::sleep_for(150ms);
        const int before = completed.load();
        const bool pending = sender.wait_for(150ms) == std::future_status::timeout;
        const bool stalled = before > 0 && completed.load() == before && started.load() > before;
        const auto close_start = std::chrono::steady_clock::now();
        CHECK_EQ(s->Close(), Errno::SUCCESS);
        CHECK(std::chrono::steady_clock::now() - close_start < 2s);
        CHECK(sender.wait_for(2s) == std::future_status::ready);
        const auto error = sender.get();
        CHECK(pending && stalled);
        CHECK(error == Errno::BADF || error == Errno::NETDOWN);
        f.NoViolations();
    }
}

void ShutdownRacesReceiverAdmission() {
    Fixture f;
    for (int i = 0; i < 100; ++i) {
        auto s = f.Open(f.b);
        CHECK_EQ(s->Bind(Endpoint({}, static_cast<u16>(54000 + i))), Errno::SUCCESS);
        std::atomic<bool> start{false};
        auto receiver = std::async(std::launch::async, [&] {
            while (!start.load()) { std::this_thread::yield(); }
            Bytes buffer(16);
            return RecvInto(*s, buffer);
        });
        start = true;
        if (i % 2) { std::this_thread::yield(); }
        CHECK_EQ(s->Shutdown(ShutdownHow::RD), Errno::NOTCONN);
        const bool woke = receiver.wait_for(500ms) == std::future_status::ready;
        if (!woke) { (void)s->Close(); } // make a failing probe terminate rather than hang
        const auto result = receiver.get();
        CHECK(woke);
        CHECK_EQ(result, (std::pair<s32, Errno>{0, Errno::SUCCESS}));
        Bytes buffer(16);
        CHECK_EQ(RecvInto(*s, buffer, FLAG_MSG_DONTWAIT), (std::pair<s32, Errno>{0, Errno::SUCCESS}));
        CHECK(Readable(PollOnce(*s, 0)));
    }
    f.NoViolations();
}

void SocketOptionsMatchDatagramAndClose() {
    Fixture f;
    auto s = f.Open(f.a);
    for (bool enable : {false, true}) {
        CHECK_EQ(s->SetLinger(enable, 0), Errno::SUCCESS);
        CHECK_EQ(s->SetReuseAddr(enable), Errno::SUCCESS);
        CHECK_EQ(s->SetKeepAlive(enable), Errno::SUCCESS);
        CHECK_EQ(s->SetBroadcast(enable), Errno::SUCCESS);
    }
    CHECK_EQ(s->SetSndBuf(65536), Errno::SUCCESS);
    CHECK_EQ(s->SetRcvBuf(65536), Errno::SUCCESS);
    CHECK(s->GetPendingError() == (std::pair{Errno::SUCCESS, Errno::SUCCESS}));
    const auto broadcast = Endpoint(Ip{255, 255, 255, 255}, 50000);
    const Bytes data{1};
    CHECK_EQ(s->SendTo(0, data, &broadcast).second, Errno::NETUNREACH);
    CHECK_EQ(s->Close(), Errno::SUCCESS);
    CHECK_EQ(s->SetLinger(false, 0), Errno::BADF);
    CHECK_EQ(s->SetReuseAddr(true), Errno::BADF);
    CHECK_EQ(s->SetKeepAlive(true), Errno::BADF);
    CHECK_EQ(s->SetBroadcast(true), Errno::BADF);
    CHECK_EQ(s->SetSndBuf(65536), Errno::BADF);
    CHECK_EQ(s->SetRcvBuf(65536), Errno::BADF);
    CHECK_EQ(s->GetPendingError().second, Errno::BADF);
}

// Mirrors BSD's retained descriptor snapshot: Close may finish after the snapshot is taken
// but before Network::Poll reads GetFD. The handle must still be the same live wake-up socket.
void RetainedPollSnapshotSurvivesClose() {
    Fixture f;
    std::shared_ptr<NsoLanSocket> published = f.Open(f.b);
    CHECK_EQ(published->Bind(Endpoint({}, 53900)), Errno::SUCCESS);
    std::shared_ptr<SocketBase> snapshot = published;
    std::weak_ptr<SocketBase> lifetime = snapshot;
    const auto handle = snapshot->GetFD();
    CHECK(handle != INVALID_SOCKET);
    CHECK(snapshot->IsOpened());
    CHECK_EQ(published->Close(), Errno::SUCCESS);
    published.reset(); // descriptor removal leaves the in-flight snapshot as the last owner
    CHECK(!lifetime.expired());
    CHECK(!snapshot->IsOpened());
    CHECK_EQ(snapshot->GetFD(), handle); // deterministically catches withdrawal during Close
    std::vector<Network::PollFD> fds{{snapshot.get(), PollEvents::In, PollEvents{}}};
    const auto result = Network::Poll(fds, 1000);
    CHECK_EQ(result.second, Errno::SUCCESS);
    CHECK_EQ(result.first, 1);
    CHECK(Readable(fds[0].revents));
    CHECK((fds[0].revents & PollEvents::Nval) == PollEvents{});
    Bytes buffer(16);
    CHECK_EQ(snapshot->Recv(0, buffer), (std::pair<s32, Errno>{-1, Errno::BADF}));
    snapshot.reset();
    CHECK(lifetime.expired());
    f.NoViolations();
}

void SignalAccountingAcrossDropRejectPeekAndFullQueue() {
    Fixture f;
    auto sb = f.Open(f.b);
    CHECK(f.broker.WaitForHellos(1, 2000ms));
    CHECK_EQ(sb->Bind(Endpoint({}, 50600)), Errno::SUCCESS);
    CHECK_EQ(sb->Connect(Endpoint(AddrA, 50500)), Errno::SUCCESS);
    const std::string node = f.LastNode();
    Bytes batch;
    const auto add = [&](const Bytes& frame) { batch.insert(batch.end(), frame.begin(), frame.end()); };
    add(Mock::OracleEncode(Ip{10, 253, 1, 3}, AddrB, 7, 50600, Payload("stranger-1"))); // Drop
    add(Mock::OracleEncode(Ip{10, 253, 1, 4}, AddrB, 7, 50600, Payload("stranger-2"))); // Drop
    add(Mock::OracleEncode(AddrA, AddrB, 50500, 50601, Payload("wrong-port")));        // Reject
    add(Mock::OracleEncode(AddrA, AddrB, 50500, 50600, Payload("good")));              // Deliver
    CHECK_EQ(f.broker.Inject(node, batch), 0);
    CHECK(Readable(PollOnce(*sb, 2000)));
    std::this_thread::sleep_for(200ms); // let every record arrive
    Bytes buffer(32);
    // Peek: the strangers are dropped on the way (and their signal bytes consumed); the rejected
    // record stays queued with its byte, exactly as a non-peek receive would find it.
    CHECK_EQ(RecvInto(*sb, buffer, FLAG_MSG_PEEK), (std::pair<s32, Errno>{-1, Errno::INVAL}));
    CHECK_EQ(RecvInto(*sb, buffer, FLAG_MSG_PEEK), (std::pair<s32, Errno>{-1, Errno::INVAL}));
    CHECK(Readable(PollOnce(*sb, 50)));
    CHECK_EQ(RecvInto(*sb, buffer), (std::pair<s32, Errno>{-1, Errno::INVAL})); // consumed now
    CHECK(Readable(PollOnce(*sb, 50)));
    CHECK_EQ(RecvInto(*sb, buffer, FLAG_MSG_PEEK), (std::pair<s32, Errno>{4, Errno::SUCCESS}));
    CHECK(Readable(PollOnce(*sb, 50)));
    CHECK_EQ(RecvInto(*sb, buffer), (std::pair<s32, Errno>{4, Errno::SUCCESS}));
    CHECK_EQ(PollOnce(*sb, 50), PollEvents{}); // signal bytes and frames consumed one for one
    // Full queue: 600 valid records, 512 kept, the rest dropped without a signal byte.
    Bytes burst;
    for (int i = 0; i < 600; ++i) {
        const Bytes one = Mock::OracleEncode(AddrA, AddrB, 50500, 50600, Payload("burst"));
        burst.insert(burst.end(), one.begin(), one.end());
    }
    CHECK_EQ(f.broker.Inject(node, burst), 0);
    std::this_thread::sleep_for(500ms);
    int received = 0;
    while (RecvInto(*sb, buffer, FLAG_MSG_DONTWAIT).second == Errno::SUCCESS) {
        ++received;
    }
    CHECK_EQ(received, 512);
    CHECK_EQ(PollOnce(*sb, 50), PollEvents{});
    // Delivery continues normally after the drops.
    CHECK_EQ(f.broker.Inject(node, Mock::OracleEncode(AddrA, AddrB, 50500, 50600, Payload("after"))), 0);
    CHECK(Readable(PollOnce(*sb, 2000)));
    CHECK_EQ(RecvInto(*sb, buffer), (std::pair<s32, Errno>{5, Errno::SUCCESS}));
    CHECK_EQ(PollOnce(*sb, 50), PollEvents{});
}

void ReinitializeAfterCloseReusesTheObject() {
    Fixture f;
    auto s = f.Open(f.b);
    CHECK_EQ(s->Bind(Endpoint({}, 50600)), Errno::SUCCESS);
    CHECK_EQ(s->Close(), Errno::SUCCESS);
    CHECK_EQ(s->Initialize(Domain::INET, Type::DGRAM, Protocol::UDP), Errno::SUCCESS);
    CHECK_EQ(s->Bind(Endpoint({}, 50600)), Errno::SUCCESS); // the port was released by the unbind
    CHECK_EQ(PollOnce(*s, 50), PollEvents{});
    CHECK_EQ(s->Close(), Errno::SUCCESS);
    CHECK(NodesIn(f.directory).empty());
    f.NoViolations();
}

struct Case {
    const char* name;
    void (*run)();
};

const Case cases[] = {
    {"PrivateDirectoryCreationNeverAdoptsExistingPaths", PrivateDirectoryCreationNeverAdoptsExistingPaths},
    {"InitializeRejectsWrongTypesAndNonPrivateDirectories", InitializeRejectsWrongTypesAndNonPrivateDirectories},
    {"InitializeCreatesNodeSaysHelloAndIsNotReadable", InitializeCreatesNodeSaysHelloAndIsNotReadable},
    {"BindRegistersAndRoutesLiteralBytes", BindRegistersAndRoutesLiteralBytes},
    {"ZeroLengthAndMaximumPayloads", ZeroLengthAndMaximumPayloads},
    {"TruncationPeekAndNonBlocking", TruncationPeekAndNonBlocking},
    {"ConnectedSocketFiltersOtherSources", ConnectedSocketFiltersOtherSources},
    {"RecordsNotForThisSocketAreRejected", RecordsNotForThisSocketAreRejected},
    {"CorruptStreamFromBrokerIsNetworkDown", CorruptStreamFromBrokerIsNetworkDown},
    {"BrokerClosingTheStreamIsNetworkDown", BrokerClosingTheStreamIsNetworkDown},
    {"ShutdownWakesBlockedReceiverAndPoll", ShutdownWakesBlockedReceiverAndPoll},
    {"CloseWakesBlockedReceiverUnbindsAndRemovesNode", CloseWakesBlockedReceiverUnbindsAndRemovesNode},
    {"ReceiveTimeoutReturnsAgain", ReceiveTimeoutReturnsAgain},
    {"ControlRefusalsTimeoutsAndBindRules", ControlRefusalsTimeoutsAndBindRules},
    {"HelloRefusedLeavesNoNode", HelloRefusedLeavesNoNode},
    {"DestinationValidation", DestinationValidation},
    {"QueueIsBounded", QueueIsBounded},
    {"ImplicitBindOnSendAndSameGuestPortAcrossInstances", ImplicitBindOnSendAndSameGuestPortAcrossInstances},
    {"CloseWhileBlockedInRecvRepeated", CloseWhileBlockedInRecvRepeated},
    {"CloseRacingInFlightSendRepeated", CloseRacingInFlightSendRepeated},
    {"CloseWhileBlockedInPollRepeated", CloseWhileBlockedInPollRepeated},
    {"CloseInterruptsBackpressuredSend", CloseInterruptsBackpressuredSend},
    {"ShutdownRacesReceiverAdmission", ShutdownRacesReceiverAdmission},
    {"SocketOptionsMatchDatagramAndClose", SocketOptionsMatchDatagramAndClose},
    {"RetainedPollSnapshotSurvivesClose", RetainedPollSnapshotSurvivesClose},
    {"SignalAccountingAcrossDropRejectPeekAndFullQueue", SignalAccountingAcrossDropRejectPeekAndFullQueue},
    {"ReinitializeAfterCloseReusesTheObject", ReinitializeAfterCloseReusesTheObject},
};

} // Anonymous namespace

int main(int argc, char** argv) {
    // Read-only production-predicate probe for an elevated-created fixture, launched again
    // under a normal interactive token by the external Windows validation harness.
    if (argc == 3 && std::strcmp(argv[1], "--check-private-directory") == 0) {
        const bool accepted = Network::NsoLan::IsPrivateDirectory(argv[2]);
        std::printf("private_directory_accepted=%s\n", accepted ? "true" : "false");
        return accepted ? 0 : 1;
    }
    if (argc != 1) {
        return 2;
    }
    Network::NetworkInstance winsock; // WSAStartup, as the emulator does
    int failures = 0;
    int passed = 0;
    for (const Case& c : cases) {
        TestSupport::ClearLog();
        try {
            c.run();
            std::printf("PASS %s\n", c.name);
            ++passed;
        } catch (const TestFailure& failure) {
            std::printf("FAIL %s: %s\n", c.name, failure.message.c_str());
            ++failures;
        } catch (const std::exception& error) {
            std::printf("FAIL %s: exception %s\n", c.name, error.what());
            ++failures;
        }
    }
    std::printf("%d passed, %d failed, %zu production soft-asserts\n", passed, failures, TestSupport::SoftAssertCount());
    return failures == 0 && TestSupport::SoftAssertCount() == 0 ? 0 : 1;
}
