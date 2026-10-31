// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/internal_network/nso_lan.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>

namespace Network::NsoLan {
namespace {
std::optional<IPv4Address> ParsePrivateAddress(std::string_view text) {
    IPv4Address result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto dot = text.find('.');
        const auto part = text.substr(0, dot);
        unsigned value{};
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || error != std::errc{} || end != part.data() + part.size() ||
            value > 255 || (part.size() > 1 && part.front() == '0') ||
            (i < 3) != (dot != std::string_view::npos)) {
            return std::nullopt;
        }
        result[i] = static_cast<u8>(value);
        if (dot != std::string_view::npos) {
            text.remove_prefix(dot + 1);
        }
    }
    if (result[0] == 10 || result[0] == 127 ||
        (result[0] == 172 && result[1] >= 16 && result[1] <= 31) ||
        (result[0] == 192 && result[1] == 168)) {
        return result;
    }
    return std::nullopt;
}

std::string NormalizeHost(std::string_view host) {
    std::string result{host};
    if (!result.empty() && result.back() == '.') {
        result.pop_back();
    }
    for (auto& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

bool Contains(const Configuration& config, const IPv4Address& address) {
    return std::find(config.allowed_ips.begin(), config.allowed_ips.end(), address) !=
           config.allowed_ips.end();
}

struct State {
    bool requested = false;
    std::optional<Configuration> config;
};

struct AutomaticState {
    std::mutex mutex;
    std::optional<Configuration> config;
    std::atomic<bool> ready{false};
};

AutomaticState& GetAutomaticState() {
    static AutomaticState state;
    return state;
}

const State& GetState() {
    static const State state = [] {
        State value;
        const char* path = std::getenv("EDEN_NSO_LAN_CONFIG");
        value.requested = path != nullptr;
        if (!path || !*path) {
            return value;
        }
        std::ifstream input{path, std::ios::binary};
        if (!input) {
            return value;
        }
        std::string text(65537, '\0');
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(input.gcount()));
        if (text.size() <= 65536) {
            value.config = ParseConfiguration(text);
            if (value.config && value.config->automatic_transport) {
                value.config.reset(); // The managed transport cannot be activated by a legacy file.
            }
        }
        return value;
    }();
    return state;
}
} // namespace

std::optional<Configuration> ParseConfiguration(std::string_view text) {
    try {
        const auto json = nlohmann::json::parse(text);
        if (!json.at("account_id").is_number_unsigned() || !json.at("dns").is_object() ||
            !json.at("allowed_ips").is_array()) {
            return std::nullopt;
        }
        Configuration config{.account_id = json.at("account_id").get<u64>(),
                             .dns = {}, .allowed_ips = {}, .ports = {},
                             .automatic_transport = {}};
        if (config.account_id == 0 || config.account_id > std::numeric_limits<u32>::max()) {
            return std::nullopt;
        }
        for (const auto& item : json.at("allowed_ips")) {
            const auto ip = ParsePrivateAddress(item.get<std::string>());
            if (!ip) {
                return std::nullopt;
            }
            config.allowed_ips.push_back(*ip);
        }
        if (config.allowed_ips.empty()) {
            return std::nullopt;
        }
        for (const auto& [host, value] : json.at("dns").items()) {
            const auto address = value.get<std::string>();
            const auto ip = ParsePrivateAddress(address);
            const auto name = NormalizeHost(host);
            if (!ip || !Contains(config, *ip) || name.empty() ||
                name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-.") !=
                    std::string::npos || !config.dns.emplace(name, address).second) {
                return std::nullopt;
            }
        }
        if (json.contains("port_redirects")) {
            if (!json.at("port_redirects").is_array()) {
                return std::nullopt;
            }
            for (const auto& item : json.at("port_redirects")) {
                const auto ip = ParsePrivateAddress(item.at("address").get<std::string>());
                if (!item.at("from").is_number_unsigned() || !item.at("to").is_number_unsigned()) {
                    return std::nullopt;
                }
                const auto from = item.at("from").get<u64>();
                const auto to = item.at("to").get<u64>();
                if (!ip || !Contains(config, *ip) || from == 0 || from > 65535 ||
                    to == 0 || to > 65535 ||
                    std::any_of(config.ports.begin(), config.ports.end(), [&](const auto& entry) {
                        return entry.address == *ip && entry.from == from;
                    })) {
                    return std::nullopt;
                }
                std::optional<IPv4Address> target;
                if (item.contains("target_address")) {
                    target = ParsePrivateAddress(item.at("target_address").get<std::string>());
                    if (!target || !Contains(config, *target)) {
                        return std::nullopt;
                    }
                }
                config.ports.push_back({*ip, static_cast<u16>(from), static_cast<u16>(to), target});
            }
        }
        if (json.contains("auto_transport")) {
            const auto& transport = json.at("auto_transport");
            const auto address = ParsePrivateAddress(transport.at("instance_ip").get<std::string>());
            const auto control = transport.at("control_socket").get<std::string>();
            const auto datagram = transport.at("datagram_socket").get<std::string>();
            const std::filesystem::path control_path{control}, datagram_path{datagram};
            if (!address || (*address)[0] != 10 || (*address)[1] != 253 ||
                *address == IPv4Address{10, 253, 0, 0} ||
                *address == IPv4Address{10, 253, 255, 255} ||
                !Contains(config, *address) || control.empty() || datagram.empty() ||
                control.size() >= 108 || datagram.size() >= 108 ||
                control.find('\0') != std::string::npos || datagram.find('\0') != std::string::npos ||
                !control_path.is_absolute() || !datagram_path.is_absolute() ||
                control_path.lexically_normal() != control_path ||
                datagram_path.lexically_normal() != datagram_path ||
                control_path.parent_path() != datagram_path.parent_path() ||
                control_path.filename() != "control.sock" || datagram_path.filename() != "data.sock") {
                return std::nullopt;
            }
            for (const auto& redirect : config.ports) {
                if (redirect.target_address != IPv4Address{127, 0, 0, 1} ||
                    (redirect.address != IPv4Address{10, 254, 0, 1} &&
                     redirect.address != IPv4Address{10, 254, 0, 2})) {
                    return std::nullopt;
                }
            }
            config.automatic_transport = AutomaticTransport{control, datagram, *address};
        }
        return config;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

bool IsRequested() {
    return GetAutomaticState().ready.load(std::memory_order_acquire) || GetState().requested;
}

const std::optional<Configuration>& GetConfiguration() {
    const auto& automatic = GetAutomaticState();
    if (automatic.ready.load(std::memory_order_acquire)) {
        return automatic.config;
    }
    return GetState().config;
}

bool InstallAutomaticConfiguration(std::string_view text, std::string_view expected_directory) {
    if (GetState().requested || text.size() > 65536) {
        return false;
    }
    auto config = ParseConfiguration(text);
    if (!config || !config->automatic_transport || expected_directory.empty()) {
        return false;
    }
    // The runtime writes native paths; the launcher may hand over the same directory with the
    // other separator (Qt on Windows). Compare the normalized preferred forms.
    const auto published_directory = std::filesystem::path{config->automatic_transport->control_socket}
                                         .parent_path()
                                         .lexically_normal()
                                         .make_preferred();
    const auto expected = std::filesystem::path{expected_directory}.lexically_normal().make_preferred();
    if (published_directory != expected) {
        return false;
    }
    auto& automatic = GetAutomaticState();
    const std::lock_guard guard{automatic.mutex};
    if (automatic.ready.load(std::memory_order_relaxed)) {
        return false;
    }
    automatic.config = std::move(config);
    automatic.ready.store(true, std::memory_order_release);
    return true;
}

const AutomaticTransport* GetAutomaticTransport() {
    const auto& config = GetConfiguration();
    return config && config->automatic_transport ? &*config->automatic_transport : nullptr;
}

std::optional<std::string> ResolveHost(std::string_view host) {
    const auto& config = GetConfiguration();
    if (!config) {
        return std::nullopt;
    }
    const auto name = NormalizeHost(host);
    if (const auto it = config->dns.find(name); it != config->dns.end()) {
        return it->second;
    }
    const auto literal = ParsePrivateAddress(name);
    if (literal && Contains(*config, *literal)) {
        return name;
    }
    return std::nullopt;
}

std::string ResolveServiceName(std::string_view service_name) {
    const auto& config = GetConfiguration();
    std::string result{service_name};
    if (!config) {
        return result;
    }
    const auto environment = result.find("-%.");
    if (environment == std::string::npos) {
        return result;
    }
    result.replace(environment, 3, "-lp1.");
    // NSD substitution must not authorize a hostname that DNS was not configured to resolve.
    return config->dns.contains(NormalizeHost(result)) ? result : std::string{service_name};
}

std::optional<SockAddrIn> RouteDestination(SockAddrIn address, bool datagram) {
    if (!IsRequested()) {
        return address;
    }
    const auto& config = GetConfiguration();
    if (!config || address.family != Domain::INET) {
        return std::nullopt;
    }
    const bool virtual_peer = config->automatic_transport && address.ip[0] == 10 &&
                              address.ip[1] == 253;
    // Logical peer addresses are delivered only through the datagram broker,
    // never passed to the host's native TCP routing table.
    if ((virtual_peer && !datagram) || (!virtual_peer && !Contains(*config, address.ip))) {
        return std::nullopt;
    }
    if (config->automatic_transport && datagram) {
        return address;
    }
    for (const auto& entry : config->ports) {
        if (entry.address == address.ip && entry.from == address.portno) {
            address.portno = entry.to;
            if (entry.target_address) {
                address.ip = *entry.target_address;
            }
            return address;
        }
    }
    if (config->automatic_transport) {
        return std::nullopt; // Native TCP can reach only the explicitly mapped local proxies.
    }
    return address;
}
} // namespace Network::NsoLan
