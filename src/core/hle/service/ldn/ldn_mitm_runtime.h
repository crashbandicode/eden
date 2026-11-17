// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "core/hle/service/ldn/ldn_mitm_codec.h"

namespace Service::LDN::Mitm {
inline constexpr std::uint16_t Port = 11452;
using IPv4 = std::array<std::uint8_t, 4>;
struct Configuration {
    std::string interface_name;
    IPv4 address{};
    IPv4 netmask{};
    std::uint16_t port{Port};
    bool operator==(const Configuration&) const = default;
};
struct Status {
    bool running{};
    Configuration configuration;
    std::string role{"idle"};
    std::size_t peers{};
    std::size_t pending_commands{};
    std::uint64_t received{};
    std::uint64_t sent{};
    std::uint64_t malformed{};
    std::string last_error;
};
struct Event {
    enum class Kind { Message, PeerClosed, Stopped };
    Kind kind;
    std::uint8_t slot{}; // host owns 1..7; upstream host is slot 0
    IPv4 peer{};
    Message message{};
    std::uint64_t connection{};
};

// System owns one runtime. Its I/O thread exclusively owns socket/session state.
// Sends are posted, never wait for callbacks, and may be called under the guest
// packet mutex. Stop/Detach join the thread and must be called outside that mutex.
class Runtime {
public:
    using Receiver = std::function<void(const Event&)>;
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    bool Start(const Configuration& configuration, std::string& error);
    void Stop();
    Status GetStatus() const;
    bool Attach(const void* owner, Receiver receiver, std::string& error);
    void Detach(const void* owner);
    bool Scan(std::string& error);
    bool Connect(IPv4 host, std::vector<std::uint8_t> node, std::string& error);
    void Leave();
    void PublishNetwork(std::optional<std::vector<std::uint8_t>> network);
    void ClosePeer(std::uint8_t slot, std::uint64_t connection = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Eden interface selection, implemented separately from the portable runtime.
std::optional<Configuration> SelectedConfiguration(std::string& error);
bool ValidateNetwork(std::span<const std::uint8_t> network, std::string& error);
bool ValidateNode(std::span<const std::uint8_t> node, std::string& error);
} // namespace Service::LDN::Mitm
