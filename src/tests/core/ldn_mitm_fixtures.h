// SPDX-License-Identifier: GPL-3.0-or-later
// Original synthetic fixtures from the pinned protocol facts, no game data.
#pragma once
#include <array>
#include <cstddef>
#include <string_view>

namespace CodecFixtures {
struct Golden {
    std::string_view name;
    unsigned type;
    std::size_t payload_size;
    std::string_view hex;
};
inline constexpr std::array<Golden, 4> Frames{{
    {"Scan", 0, 0, "001445110000000000000000"},
    {"ScanResp", 1, 1152, "0014451101010a008004000000ff00ff00ff00ff007f"},
    {"Connect", 2, 64, "001445110201020040000000003f"},
    {"SyncNetwork", 3, 1152, "0014451103010a008004000000ff00ff00ff00ff007f"},
}};
} // namespace CodecFixtures
