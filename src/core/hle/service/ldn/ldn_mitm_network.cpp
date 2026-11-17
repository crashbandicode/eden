// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/hle/service/ldn/ldn_mitm_runtime.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"

namespace Service::LDN::Mitm {
std::optional<Configuration> SelectedConfiguration(std::string& error) {
    error.clear();
    const auto selected = Network::GetSelectedNetworkInterface();
    if (!selected) {
        error = "Select an available IPv4 LAN interface in Network settings";
        return std::nullopt;
    }
    return Configuration{selected->name, Network::TranslateIPv4(selected->ip_address),
                         Network::TranslateIPv4(selected->subnet_mask)};
}
} // namespace Service::LDN::Mitm
