// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "common/socket_types.h"

namespace Network::NsoLan {

// A process-local test fixture. Configuration is read once, before the first guest request.
// No credentials or token material are stored in this file.
struct PortRedirect {
    IPv4Address address;
    u16 from;
    u16 to;
    std::optional<IPv4Address> target_address;
};

struct AutomaticTransport {
    std::string control_socket;
    std::string datagram_socket;
    IPv4Address instance_address;
};

struct Configuration {
    u64 account_id;
    std::map<std::string, std::string, std::less<>> dns;
    std::vector<IPv4Address> allowed_ips;
    std::vector<PortRedirect> ports;
    std::optional<AutomaticTransport> automatic_transport;
};

bool IsRequested();
const std::optional<Configuration>& GetConfiguration();
std::optional<Configuration> ParseConfiguration(std::string_view text);
// Publish once before starting the guest. The object remains immutable for this process.
// Explicit legacy fixture configuration takes precedence and cannot be overwritten.
bool InstallAutomaticConfiguration(std::string_view text, std::string_view expected_directory);
const AutomaticTransport* GetAutomaticTransport();
std::optional<std::string> ResolveHost(std::string_view host);
// Expand only the fixture's explicitly mapped NSD environment template; otherwise preserve input.
std::string ResolveServiceName(std::string_view service_name);
// A requested but invalid fixture denies traffic. Disabled mode returns the input unchanged.
std::optional<SockAddrIn> RouteDestination(SockAddrIn address, bool datagram = false);

} // namespace Network::NsoLan
