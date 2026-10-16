// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Service::Account::NsoLan {

// Synthetic fixture material only. Neither the token nor the ephemeral private key is persisted.
struct IdToken {
    std::string value;
    std::vector<unsigned char> public_key_der;
    std::int64_t expires_at;
};

std::optional<IdToken> CreateIdToken(std::uint64_t account_id, std::int64_t now,
                                    std::string_view title_version, std::uint64_t program_id);

} // namespace Service::Account::NsoLan
