// SPDX-License-Identifier: GPL-3.0-or-later
// Cross-language transport interop: the production Network::NsoLanSocket against two real Go
// runtime processes (`nsolan --auto`), over their real AF_UNIX control and datagram sockets.
//
// Scope is the UNIX transport only: no NEX, no gameplay, no emulator, no account material. The
// runtime's initialization-reply assets are explicitly fake transport-only placeholders (the
// runtime opens them but never parses them). Both children get a private XDG_RUNTIME_DIR so
// their same-host discovery cannot meet any other runtime on this machine. --local-only
// prevents either child from advertising or joining a real LAN lobby.
//
// Exit status: 0 transport and schema both pass; 4 transport passes but the runtime's ready.json
// is rejected by NsoLan::ParseConfiguration; 2 the runtime could not start/publish
// (blocked, details printed); 1 any transport failure.
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "core/internal_network/network.h"
#include "core/internal_network/nso_lan.h"
#include "core/internal_network/nso_lan_socket.h"
#include "test_support.h"

using namespace Network;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace {

struct DriverFailure {
    std::string what;
    int code = 1;
};

[[noreturn]] void Fail(const std::string& what, int code = 1) {
    throw DriverFailure{what, code};
}
void Require(bool condition, const std::string& what) {
    if (!condition) {
        Fail(what);
    }
}
void Step(const std::string& text) {
    std::printf("STEP %s\n", text.c_str());
    std::fflush(stdout);
}

std::string ErrnoName(Errno e) {
    switch (e) {
    case Errno::SUCCESS: return "SUCCESS";
    case Errno::AGAIN: return "AGAIN";
    case Errno::INVAL: return "INVAL";
    case Errno::MSGSIZE: return "MSGSIZE";
    case Errno::NETDOWN: return "NETDOWN";
    case Errno::NETUNREACH: return "NETUNREACH";
    case Errno::TIMEDOUT: return "TIMEDOUT";
    case Errno::ADDRINUSE: return "ADDRINUSE";
    case Errno::ADDRNOTAVAIL: return "ADDRNOTAVAIL";
    case Errno::OTHER: return "OTHER";
    default: return "errno#" + std::to_string(static_cast<int>(e));
    }
}

std::string Hex(const Bytes& bytes, std::size_t limit = 24) {
    std::string out = "[" + std::to_string(bytes.size()) + "]";
    for (std::size_t i = 0; i < bytes.size() && i < limit; ++i) {
        char hex[4];
        std::snprintf(hex, sizeof(hex), " %02x", bytes[i]);
        out += hex;
    }
    return out;
}

std::string ReadBounded(const std::string& path, std::size_t limit) {
    std::ifstream in{path, std::ios::binary};
    std::string text(limit, '\0');
    in.read(text.data(), static_cast<std::streamsize>(limit));
    text.resize(static_cast<std::size_t>(in.gcount()));
    return text;
}

std::optional<IPv4Address> ParseIPv4(const std::string& text) {
    in_addr address{};
    if (::inet_pton(AF_INET, text.c_str(), &address) != 1) {
        return std::nullopt;
    }
    const auto* b = reinterpret_cast<const std::uint8_t*>(&address);
    return IPv4Address{b[0], b[1], b[2], b[3]};
}

std::string Dotted(const IPv4Address& ip) {
    return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
           std::to_string(ip[3]);
}

// A private scratch tree that is always removed, even when a check throws.
class Scratch {
public:
    Scratch() {
        std::string pattern = "/tmp/elni-XXXXXX";
        Require(::mkdtemp(pattern.data()) != nullptr, "mkdtemp failed");
        root = pattern;
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    std::string Make(const std::string& name) const {
        const std::string path = root + "/" + name;
        Require(::mkdir(path.c_str(), 0700) == 0, "mkdir " + name);
        Require(::chmod(path.c_str(), 0700) == 0, "chmod " + name);
        return path;
    }
    std::string root;
};

// One real runtime process. Always terminated and reaped by the destructor.
class Child {
public:
    Child(std::string label, std::string binary, std::vector<std::string> args,
          std::vector<std::string> env, std::string stderr_path)
        : label_{std::move(label)}, binary_{std::move(binary)}, args_{std::move(args)},
          env_{std::move(env)}, stderr_path_{std::move(stderr_path)} {}
    ~Child() {
        Stop(3s);
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    void Start() {
        std::vector<char*> argv;
        argv.push_back(binary_.data());
        for (auto& arg : args_) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        std::vector<char*> envp;
        for (auto& entry : env_) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);
        const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        const int err_fd = ::open(stderr_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        Require(null_fd >= 0 && err_fd >= 0, "cannot open child stdio");
        pid_ = ::fork();
        Require(pid_ >= 0, "fork failed");
        if (pid_ == 0) {
            ::setpgid(0, 0);
            ::prctl(PR_SET_PDEATHSIG, SIGTERM);
            ::dup2(null_fd, 0);
            ::dup2(null_fd, 1);
            ::dup2(err_fd, 2);
            ::execve(binary_.c_str(), argv.data(), envp.data());
            _exit(127);
        }
        ::close(null_fd);
        ::close(err_fd);
    }

    bool Alive() {
        if (pid_ <= 0 || reaped_) {
            return false;
        }
        int status = 0;
        if (::waitpid(pid_, &status, WNOHANG) == pid_) {
            reaped_ = true;
            status_ = status;
            return false;
        }
        return true;
    }

    // SIGTERM first (the runtime stops gracefully and removes its files), then SIGKILL.
    void Stop(std::chrono::milliseconds grace) {
        if (pid_ <= 0 || reaped_) {
            return;
        }
        ::kill(pid_, SIGTERM);
        const auto deadline = Clock::now() + grace;
        while (Alive() && Clock::now() < deadline) {
            std::this_thread::sleep_for(20ms);
        }
        if (!reaped_) {
            killed_ = true;
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
            reaped_ = true;
            status_ = status;
        }
    }

    [[nodiscard]] bool ExitedNormallyWithZero() const {
        return reaped_ && !killed_ && WIFEXITED(status_) && WEXITSTATUS(status_) == 0;
    }
    [[nodiscard]] std::string Describe() const {
        if (!reaped_) {
            return label_ + ": running";
        }
        if (killed_) {
            return label_ + ": had to be SIGKILLed";
        }
        if (WIFEXITED(status_)) {
            return label_ + ": exit " + std::to_string(WEXITSTATUS(status_));
        }
        return label_ + ": signal " + std::to_string(WTERMSIG(status_));
    }
    [[nodiscard]] const std::string& StderrPath() const {
        return stderr_path_;
    }

private:
    std::string label_, binary_;
    std::vector<std::string> args_, env_;
    std::string stderr_path_;
    pid_t pid_ = -1;
    bool reaped_ = false;
    bool killed_ = false;
    int status_ = 0;
};

// The runtime's own status operation, over a raw control connection from this process (the only
// process its control socket admits).
nlohmann::json ControlStatus(const std::string& control_path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, control_path.c_str(), control_path.size());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    Require(fd >= 0, "status socket");
    timeval timeout{.tv_sec = 3, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    nlohmann::json result;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        const std::string request = "{\"op\":\"status\"}\n";
        if (::send(fd, request.data(), request.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(request.size())) {
            std::string line;
            char c;
            while (line.size() < 4096 && ::recv(fd, &c, 1, 0) == 1 && c != '\n') {
                line.push_back(c);
            }
            result = nlohmann::json::parse(line, nullptr, false);
        }
    }
    ::close(fd);
    return result;
}

struct Node {
    std::string label;
    std::string runtime_dir;
    std::unique_ptr<Child> child;
    nlohmann::json ready;
    std::string ready_text;
    std::string control_socket;
    std::string datagram_socket;
    IPv4Address ip{};
    std::unique_ptr<NsoLanSocket> guest;
};

// The independent oracle: literal payloads, written down here and nowhere else.
const Bytes kAToB{0x00, 0xFF, 0x7F, 0x80, 'E', 'D', 'E', 'N', 0x0D, 0x0A, 0x01};
const Bytes kBToA{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x02, 'L', 'A', 'N', 0xFF};
const Bytes kProbe{'E', 'L', 'N', 'I', '-', 'P', 'R', 'O', 'B', 'E'};
constexpr u16 kGuestPort = 40000;

bool Readable(SocketBase& socket, s32 timeout_ms) {
    std::vector<PollFD> fds{PollFD{&socket, PollEvents::In, PollEvents{}}};
    const auto [count, error] = Poll(fds, timeout_ms);
    Require(error == Errno::SUCCESS, "poll failed: " + ErrnoName(error));
    return count > 0 && (static_cast<u16>(fds[0].revents) & static_cast<u16>(PollEvents::In)) != 0;
}

struct Datagram {
    Bytes data;
    SockAddrIn from;
};

// Receives until a datagram equal to `expected` arrives or the window closes. Other datagrams
// (late duplicates of earlier retries) are discarded and counted.
std::optional<Datagram> AwaitDatagram(SocketBase& socket, const Bytes& expected,
                                      std::chrono::milliseconds window, int& discarded) {
    const auto deadline = Clock::now() + window;
    Bytes buffer(NsoLanWire::MaxPayloadSize + 1);
    while (Clock::now() < deadline) {
        if (!Readable(socket, 100)) {
            continue;
        }
        SockAddrIn from{};
        const auto [size, error] = socket.RecvFrom(0, buffer, &from);
        Require(error == Errno::SUCCESS, "receive failed: " + ErrnoName(error));
        Bytes data(buffer.begin(), buffer.begin() + size);
        if (data == expected) {
            return Datagram{std::move(data), from};
        }
        ++discarded;
    }
    return std::nullopt;
}

// UDP is unreliable while the runtimes are still learning about each other, so a send may be
// repeated. Success needs the exact bytes and the exact source identity.
void Exchange(const std::string& label, Node& sender, Node& receiver, const Bytes& payload,
              std::chrono::seconds budget) {
    const auto deadline = Clock::now() + budget;
    int discarded = 0;
    SockAddrIn to{.family = Domain::INET, .ip = receiver.ip, .portno = kGuestPort};
    int attempts = 0;
    while (Clock::now() < deadline) {
        ++attempts;
        const auto [sent, error] = sender.guest->SendTo(0, payload, &to);
        Require(error == Errno::SUCCESS && sent == static_cast<s32>(payload.size()),
                label + ": send failed with " + ErrnoName(error));
        if (const auto got = AwaitDatagram(*receiver.guest, payload, 400ms, discarded)) {
            Require(got->from.family == Domain::INET && got->from.ip == sender.ip &&
                        got->from.portno == kGuestPort,
                    label + ": wrong source identity " + Dotted(got->from.ip) + ":" +
                        std::to_string(got->from.portno));
            std::printf("  %s: %s delivered with exact bytes and source %s:%u (attempts=%d, "
                        "discarded=%d)\n",
                        label.c_str(), Hex(payload, 6).c_str(), Dotted(sender.ip).c_str(),
                        static_cast<unsigned>(got->from.portno), attempts, discarded);
            return;
        }
    }
    Fail(label + ": no matching datagram within " + std::to_string(budget.count()) + " s");
}

// Why would NsoLan::ParseConfiguration refuse this ready.json? Independent of that parser.
std::vector<std::string> DiagnoseSchema(const nlohmann::json& ready) {
    std::vector<std::string> findings;
    std::vector<std::string> allowed;
    for (const auto& item : ready.value("allowed_ips", nlohmann::json::array())) {
        allowed.push_back(item.get<std::string>());
    }
    const auto listed = [&](const std::string& ip) {
        return std::find(allowed.begin(), allowed.end(), ip) != allowed.end();
    };
    const std::string instance = ready.at("auto_transport").value("instance_ip", "");
    if (!listed(instance)) {
        findings.push_back("allowed_ips lacks the instance's own address " + instance);
    }
    for (const auto& redirect : ready.value("port_redirects", nlohmann::json::array())) {
        const std::string target = redirect.value("target_address", "");
        if (!listed(target)) {
            findings.push_back("allowed_ips lacks redirect target_address " + target);
            break;
        }
    }
    return findings;
}

struct Outcome {
    bool schema_a = false;
    bool schema_b = false;
    std::vector<std::string> schema_findings;
};

Outcome Run(const std::string& binary, const std::string& title_id, int ready_timeout_s) {
    Outcome outcome;
    Scratch scratch;
    const std::string xdg = scratch.Make("x");
    const std::string assets = scratch.Make("assets");
    scratch.Make("tmp");

    // Explicitly fake, transport-only placeholders for the nine initialization replies.
    for (const char* name : {"0x6e_7.bin", "0x6e_9.bin", "0x6e_10.bin", "0x6e_11.bin",
                             "0x73_21.bin", "0x73_60.bin", "0x73_61.bin", "0x73_84.bin",
                             "0x73_95.bin"}) {
        std::ofstream{assets + "/" + name} << "FAKE TRANSPORT-ONLY PLACEHOLDER " << name << "\n";
    }

    std::vector<Node> nodes(2);
    nodes[0].label = "A";
    nodes[1].label = "B";
    const std::vector<std::string> env{"XDG_RUNTIME_DIR=" + xdg, "TMPDIR=" + scratch.root + "/tmp",
                                       "PATH=/usr/bin:/bin"};
    Step("spawn two real runtime children (--auto) with private runtime dirs");
    for (auto& node : nodes) {
        node.runtime_dir = scratch.Make(node.label == "A" ? "a" : "b");
        node.child = std::make_unique<Child>(
            node.label, binary,
            std::vector<std::string>{"--auto", "--local-only", "--parent-pid", std::to_string(::getpid()),
                                     "--runtime-dir", node.runtime_dir, "--assets", assets,
                                     "--title-id", title_id},
            env, scratch.root + "/" + node.label + ".err");
        node.child->Start();
    }

    Step("wait for ready.json from both children");
    const auto deadline = Clock::now() + std::chrono::seconds{ready_timeout_s};
    const auto describe_failure = [&](Node& node) {
        std::string text = node.child->Describe();
        const std::string error_json = ReadBounded(node.runtime_dir + "/error.json", 1024);
        if (!error_json.empty()) {
            text += "; error.json=" + error_json;
        }
        std::string log = ReadBounded(node.child->StderrPath(), 1500);
        if (!log.empty()) {
            text += "; stderr tail=" + log;
        }
        return text;
    };
    while (true) {
        bool all_ready = true;
        for (auto& node : nodes) {
            if (!node.child->Alive()) {
                Fail("runtime " + node.label + " stopped before publishing ready.json: " +
                         describe_failure(node),
                     2);
            }
            all_ready = all_ready && fs::is_regular_file(node.runtime_dir + "/ready.json");
        }
        if (all_ready) {
            break;
        }
        if (Clock::now() > deadline) {
            Fail("ready.json not published within " + std::to_string(ready_timeout_s) + " s: " +
                     describe_failure(nodes[0]) + " | " + describe_failure(nodes[1]),
                 2);
        }
        std::this_thread::sleep_for(50ms);
    }

    Step("parse ready.json and run the full schema through NsoLan::ParseConfiguration");
    for (auto& node : nodes) {
        node.ready_text = ReadBounded(node.runtime_dir + "/ready.json", 65537);
        Require(!node.ready_text.empty() && node.ready_text.size() <= 65536, "ready.json size");
        node.ready = nlohmann::json::parse(node.ready_text, nullptr, false);
        Require(node.ready.is_object(), "ready.json is not a JSON object");
        const auto& transport = node.ready.at("auto_transport");
        node.control_socket = transport.at("control_socket").get<std::string>();
        node.datagram_socket = transport.at("datagram_socket").get<std::string>();
        const auto ip = ParseIPv4(transport.at("instance_ip").get<std::string>());
        Require(ip.has_value() && (*ip)[0] == 10 && (*ip)[1] == 253, "instance_ip is not in 10.253/16");
        node.ip = *ip;
        Require(node.control_socket == node.runtime_dir + "/control.sock" &&
                    node.datagram_socket == node.runtime_dir + "/data.sock",
                "transport sockets are not control.sock/data.sock in the runtime directory");
        struct stat info{};
        Require(::lstat(node.control_socket.c_str(), &info) == 0 && S_ISSOCK(info.st_mode),
                "control.sock is not a socket");
        Require(::lstat(node.datagram_socket.c_str(), &info) == 0 && S_ISSOCK(info.st_mode),
                "data.sock is not a socket");
        const bool accepted = NsoLan::ParseConfiguration(node.ready_text).has_value();
        (node.label == "A" ? outcome.schema_a : outcome.schema_b) = accepted;
        std::printf("  %s: instance_ip=%s allowed_ips=%zu port_redirects=%zu ParseConfiguration=%s\n",
                    node.label.c_str(), Dotted(node.ip).c_str(),
                    node.ready.value("allowed_ips", nlohmann::json::array()).size(),
                    node.ready.value("port_redirects", nlohmann::json::array()).size(),
                    accepted ? "ACCEPTED" : "REJECTED");
        if (!accepted) {
            for (const auto& finding : DiagnoseSchema(node.ready)) {
                if (std::find(outcome.schema_findings.begin(), outcome.schema_findings.end(),
                              node.label + ": " + finding) == outcome.schema_findings.end()) {
                    outcome.schema_findings.push_back(node.label + ": " + finding);
                }
            }
        }
    }
    Require(nodes[0].ip != nodes[1].ip, "both runtimes were given the same virtual address");
    if (outcome.schema_a) {
        Require(NsoLan::InstallAutomaticConfiguration(nodes[0].ready_text, nodes[0].runtime_dir),
                "actual configuration publication gate rejected node A");
    }

    Step("create two production NsoLanSocket instances on the same guest port");
    for (auto& node : nodes) {
        node.guest = std::make_unique<NsoLanSocket>(node.control_socket, node.datagram_socket, node.ip);
        Require(node.guest->Initialize(Domain::INET, Type::DGRAM, Protocol::UDP) == Errno::SUCCESS,
                node.label + ": Initialize failed");
        const Errno bound = node.guest->Bind(SockAddrIn{.family = Domain::INET, .ip = {}, .portno = kGuestPort});
        Require(bound == Errno::SUCCESS, node.label + ": Bind failed with " + ErrnoName(bound));
        const auto name = node.guest->GetSockName();
        Require(name.second == Errno::SUCCESS && name.first.ip == node.ip && name.first.portno == kGuestPort,
                node.label + ": GetSockName is not the virtual endpoint");
        const auto status = ControlStatus(node.control_socket);
        Require(status.is_object() && status.value("local_only", false),
                node.label + ": runtime must disable all LAN transport for this probe");
        Require(status.is_object() && status.value("ok", false) && status.value("bindings", -1) == 1 &&
                    status.value("instance_ip", "") == Dotted(node.ip),
                node.label + ": runtime does not report exactly this binding: " + status.dump());
        std::printf("  %s: bound %s:%u; runtime reports bindings=1 peers=%d leader=%s\n",
                    node.label.c_str(), Dotted(node.ip).c_str(), static_cast<unsigned>(kGuestPort),
                    status.value("peers", -1), status.value("leader", false) ? "yes" : "no");
    }
    // A second socket cannot take a guest port the same runtime already holds.
    {
        NsoLanSocket duplicate{nodes[0].control_socket, nodes[0].datagram_socket, nodes[0].ip};
        Require(duplicate.Initialize(Domain::INET, Type::DGRAM, Protocol::UDP) == Errno::SUCCESS,
                "duplicate Initialize");
        const Errno clash = duplicate.Bind(SockAddrIn{.family = Domain::INET, .ip = {}, .portno = kGuestPort});
        Require(clash == Errno::ADDRINUSE, "duplicate guest port on one runtime gave " + ErrnoName(clash) +
                                               " (expected ADDRINUSE from the runtime's port_in_use)");
        std::printf("  duplicate bind of the same port on one runtime: %s\n", ErrnoName(clash).c_str());
    }

    Step("wait for the runtimes to route A -> B (probe datagrams, no reply expected)");
    {
        const auto route_deadline = Clock::now() + 30s;
        bool routed = false;
        int probes = 0;
        SockAddrIn to{.family = Domain::INET, .ip = nodes[1].ip, .portno = kGuestPort};
        while (Clock::now() < route_deadline && !routed) {
            ++probes;
            Require(nodes[0].guest->SendTo(0, kProbe, &to).second == Errno::SUCCESS, "probe send");
            routed = Readable(*nodes[1].guest, 300);
        }
        Require(routed, "no datagram crossed A -> B within 30 s; status A=" +
                            ControlStatus(nodes[0].control_socket).dump() +
                            " B=" + ControlStatus(nodes[1].control_socket).dump());
        const auto a_status = ControlStatus(nodes[0].control_socket);
        const auto b_status = ControlStatus(nodes[1].control_socket);
        Require(a_status.value("local_only", false) && b_status.value("local_only", false) &&
                    a_status.value("peers", -1) == 1 && b_status.value("peers", -1) == 1 &&
                    a_status.value("leader", false) != b_status.value("leader", false),
                "the isolated pair must contain exactly two members and one coordinator");
        std::printf("  route established after %d probe(s)\n", probes);
        // Drain probe leftovers until both sockets have been quiet for 400 ms (also proves that
        // Poll times out on an idle socket).
        for (auto& node : nodes) {
            Bytes buffer(NsoLanWire::MaxPayloadSize + 1);
            int drained = 0;
            while (Readable(*node.guest, 400)) {
                SockAddrIn from{};
                const auto [size, error] = node.guest->RecvFrom(0, buffer, &from);
                Require(error == Errno::SUCCESS, "drain failed");
                (void)size;
                ++drained;
            }
            std::printf("  %s: drained %d probe datagram(s); idle poll timed out as expected\n",
                        node.label.c_str(), drained);
        }
    }

    Step("exchange independent literal bytes in both directions (identity checked)");
    Exchange("A->B literal", nodes[0], nodes[1], kAToB, 20s);
    Exchange("B->A literal", nodes[1], nodes[0], kBToA, 20s);
    Exchange("A->B zero-length", nodes[0], nodes[1], Bytes{}, 20s);
    {
        Bytes largest(NsoLanWire::MaxPayloadSize);
        for (std::size_t i = 0; i < largest.size(); ++i) {
            largest[i] = static_cast<std::uint8_t>(i * 13 + 5);
        }
        Exchange("B->A 65483-byte payload (65507-byte frame)", nodes[1], nodes[0], largest, 20s);
        Bytes too_large(NsoLanWire::MaxPayloadSize + 1, 0x42);
        SockAddrIn to{.family = Domain::INET, .ip = nodes[1].ip, .portno = kGuestPort};
        const auto refused = nodes[0].guest->SendTo(0, too_large, &to);
        Require(refused.first == -1 && refused.second == Errno::MSGSIZE,
                "65484-byte payload was not refused locally with MSGSIZE");
        std::printf("  65484-byte payload refused locally: MSGSIZE\n");
    }

    Step("close the sockets, then confirm the runtimes saw the unbind");
    for (auto& node : nodes) {
        Require(node.guest->Close() == Errno::SUCCESS, node.label + ": Close failed");
        const auto status = ControlStatus(node.control_socket);
        Require(status.is_object() && status.value("ok", false) && status.value("bindings", -1) == 0,
                node.label + ": runtime still reports bindings after Close: " + status.dump());
        std::size_t guest_files = 0;
        for (const auto& entry : fs::directory_iterator(node.runtime_dir)) {
            guest_files += entry.path().filename().string().rfind("eln-", 0) == 0 ? 1 : 0;
        }
        Require(guest_files == 0, node.label + ": guest socket file left behind");
        std::printf("  %s: bindings=0, no guest socket files left\n", node.label.c_str());
    }

    Step("stop the runtimes (SIGTERM) and verify they clean up after themselves");
    for (auto& node : nodes) {
        node.child->Stop(10s);
        Require(node.child->ExitedNormallyWithZero(), "runtime did not stop cleanly: " + node.child->Describe());
        for (const char* name : {"control.sock", "data.sock", "ready.json"}) {
            Require(!fs::exists(node.runtime_dir + "/" + name),
                    node.label + ": runtime left " + name + " behind");
        }
        std::printf("  %s: exit 0, control.sock/data.sock/ready.json removed\n", node.label.c_str());
    }
    Require(TestSupport::SoftAssertCount() == 0, "a production assert fired");
    return outcome;
}

} // Anonymous namespace

int main(int argc, char** argv) {
    std::string binary;
    std::string title_id = "01006A800016E000";
    int ready_timeout_s = 40;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--runtime-binary" && i + 1 < argc) {
            binary = argv[++i];
        } else if (arg == "--title-id" && i + 1 < argc) {
            title_id = argv[++i];
        } else if (arg == "--ready-timeout" && i + 1 < argc) {
            ready_timeout_s = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: interop_probe --runtime-binary PATH [--title-id ID] "
                                 "[--ready-timeout S]\n");
            return 64;
        }
    }
    if (binary.empty() || !fs::is_regular_file(binary)) {
        std::fprintf(stderr, "interop_probe: --runtime-binary must name an existing file\n");
        return 64;
    }

    const NetworkInstance network; // the interrupt pipe Network::Poll uses
    ::unsetenv("EDEN_NSO_LAN_CONFIG"); // Hermetic publication gate; never consume a caller's fixture.
    try {
        const Outcome outcome = Run(binary, title_id, ready_timeout_s);
        const bool schema_ok = outcome.schema_a && outcome.schema_b;
        std::printf("RESULT transport=PASS schema=%s\n", schema_ok ? "ACCEPTED" : "REJECTED");
        for (const auto& finding : outcome.schema_findings) {
            std::printf("SCHEMA-BLOCK %s\n", finding.c_str());
        }
        if (!schema_ok) {
            return 4;
        }
        return 0;
    } catch (const DriverFailure& failure) {
        std::printf("RESULT %s: %s\n", failure.code == 2 ? "BLOCKED" : "FAIL", failure.what.c_str());
        return failure.code;
    } catch (const std::exception& error) {
        std::printf("RESULT FAIL: unexpected exception: %s\n", error.what());
        return 1;
    }
}
