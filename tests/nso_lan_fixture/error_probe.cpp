// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstdlib>
#include <iostream>
#include <utility>
#include "common/assert.h"
#include "common/logging.h"
#include "core/hle/service/sockets/sockets.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"
#include "core/internal_network/nso_lan.h"
#include "core/internal_network/sockets.h"

namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*,
                       fmt::string_view, const fmt::format_args&) {}
}
void AssertFailSoftImpl() { std::abort(); }
[[noreturn]] void AssertFatalImpl() { std::abort(); }

namespace Network {
std::optional<NetworkInterface> GetSelectedNetworkInterface() { return std::nullopt; }
}

int main() {
    using E = Network::Errno;
    // Concrete guest ABI values, independent of the production switch.
    constexpr std::array cases{
        std::pair{E::SUCCESS, 0u}, std::pair{E::BADF, 9u}, std::pair{E::AGAIN, 11u},
        std::pair{E::INVAL, 22u}, std::pair{E::MFILE, 24u}, std::pair{E::PIPE, 32u},
        std::pair{E::MSGSIZE, 90u}, std::pair{E::NETDOWN, 100u}, std::pair{E::NETUNREACH, 101u},
        std::pair{E::CONNABORTED, 103u}, std::pair{E::CONNRESET, 104u},
        std::pair{E::ISCONN, 106u}, std::pair{E::NOTCONN, 107u},
        std::pair{E::TIMEDOUT, 110u}, std::pair{E::CONNREFUSED, 111u},
        std::pair{E::HOSTUNREACH, 113u}, std::pair{E::INPROGRESS, 115u}, std::pair{E::OTHER, 5u},
        std::pair{E::ACCES, 13u}, std::pair{E::ADDRINUSE, 98u}, std::pair{E::ADDRNOTAVAIL, 99u}};
    for (const auto& [error, expected] : cases) {
        if (static_cast<u32>(Service::Sockets::Translate(error)) != expected) {
            std::cerr << "guest errno mismatch\n";
            return 1;
        }
    }
    if (Network::NsoLan::IsRequested()) return 2;
    Network::NetworkInstance instance;
    Network::Socket first, second;
    if (first.Initialize(Network::Domain::INET, Network::Type::DGRAM, Network::Protocol::UDP) != E::SUCCESS ||
        second.Initialize(Network::Domain::INET, Network::Type::DGRAM, Network::Protocol::UDP) != E::SUCCESS) return 3;
    if (first.Bind({.family=Network::Domain::INET, .ip={127,0,0,1}, .portno=0}) != E::SUCCESS) return 4;
    const auto [endpoint, name_error] = first.GetSockName();
    if (name_error != E::SUCCESS || endpoint.portno == 0) return 5;
    const auto conflict = second.Bind(endpoint);
    if (conflict != E::ADDRINUSE || static_cast<u32>(Service::Sockets::Translate(conflict)) != 98) return 6;
    constexpr std::array<u8, 4> payload{0x12, 0x34, 0x00, 0xff};
    const auto [sent, send_error] = second.SendTo(0, payload, &endpoint);
    if (sent != 4 || send_error != E::SUCCESS) return 7;
    std::vector<Network::PollFD> fds{{&first, Network::PollEvents::In, {}}};
    const auto [ready, poll_error] = Network::Poll(fds, 1000);
    if (ready != 1 || poll_error != E::SUCCESS) return 8;
    std::array<u8, 4> received{};
    const auto [size, receive_error] = first.RecvFrom(0, received, nullptr);
    if (size != 4 || receive_error != E::SUCCESS || received != payload) return 9;
    std::cout << "PASS: " << cases.size() << " guest errno mappings\n";
    std::cout << "PASS: disabled-mode native UDP bind conflict, poll and delivery\n";
}
