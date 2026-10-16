// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/internal_network/nso_lan.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
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
                             .dns = {}, .allowed_ips = {}, .ports = {}};
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
                config.ports.push_back({*ip, static_cast<u16>(from), static_cast<u16>(to)});
            }
        }
        return config;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

bool IsRequested() {
    return GetState().requested;
}

const std::optional<Configuration>& GetConfiguration() {
    return GetState().config;
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

std::optional<SockAddrIn> RouteDestination(SockAddrIn address) {
    if (!IsRequested()) {
        return address;
    }
    const auto& config = GetConfiguration();
    if (!config || address.family != Domain::INET || !Contains(*config, address.ip)) {
        return std::nullopt;
    }
    for (const auto& entry : config->ports) {
        if (entry.address == address.ip && entry.from == address.portno) {
            address.portno = entry.to;
            break;
        }
    }
    return address;
}
} // namespace Network::NsoLan
