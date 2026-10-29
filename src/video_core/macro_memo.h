// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace Tegra {

using MacroMemoPairs = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

// The effects of native macro calls that depend on nothing but their start address, their
// parameters and data RAM words they read (no register or memory reads), so that a repeated
// call replays its data RAM and method writes instead of being interpreted. In title 1's 3D
// scenes ~96% of the ~1,000,000 calls a second repeat an earlier one exactly, and they run 7
// instructions on average: the per-call interpreter cost, not the instructions, was the time.
// `Map` maps a u64 (the call's hash) to a u32 (its record's offset in the arena).
template <typename Map>
class BasicMacroMemo {
public:
    /// A remembered call's effects: (address, value) pairs, flattened.
    struct Effects {
        std::span<const std::uint32_t> data_ram_writes;
        std::span<const std::uint32_t> writes; ///< (method, value) in order
    };

    /// An earlier call with this start and these parameters whose data RAM reads still hold.
    bool Find(std::uint32_t start, std::span<const std::uint32_t> parameters,
              std::span<const std::uint32_t> data_ram, Effects& effects) const {
        const auto it = entries.find(Hash(start, parameters));
        if (it == entries.end()) {
            return false;
        }
        // Record: start, parameter count, read count, data RAM write count, write count, then
        // the parameters and the (address, value) pairs.
        const std::uint32_t* record = arena.data() + it->second;
        const std::uint32_t num_parameters = record[1], num_reads = record[2];
        const std::uint32_t num_data_ram_writes = record[3], num_writes = record[4];
        if (record[0] != start || num_parameters != parameters.size() ||
            !std::equal(parameters.begin(), parameters.end(), record + HeaderWords)) {
            return false; // another call with the same hash
        }
        const std::uint32_t* reads = record + HeaderWords + num_parameters;
        for (std::uint32_t i = 0; i < num_reads; ++i) {
            if (data_ram[reads[i * 2]] != reads[i * 2 + 1]) {
                return false;
            }
        }
        const std::uint32_t* data_ram_writes = reads + num_reads * 2;
        effects.data_ram_writes = {data_ram_writes, num_data_ram_writes * 2};
        effects.writes = {data_ram_writes + num_data_ram_writes * 2, num_writes * 2};
        return true;
    }

    void Store(std::uint32_t start, std::span<const std::uint32_t> parameters,
               const MacroMemoPairs& data_ram_reads, const MacroMemoPairs& data_ram_writes,
               const MacroMemoPairs& writes) {
        if (arena.size() >= MaxArenaWords) {
            arena.clear();
            entries.clear();
        }
        const auto offset = static_cast<std::uint32_t>(arena.size());
        arena.push_back(start);
        arena.push_back(static_cast<std::uint32_t>(parameters.size()));
        arena.push_back(static_cast<std::uint32_t>(data_ram_reads.size()));
        arena.push_back(static_cast<std::uint32_t>(data_ram_writes.size()));
        arena.push_back(static_cast<std::uint32_t>(writes.size()));
        arena.insert(arena.end(), parameters.begin(), parameters.end());
        for (const MacroMemoPairs* pairs : {&data_ram_reads, &data_ram_writes, &writes}) {
            for (const auto& [address, value] : *pairs) {
                arena.push_back(address);
                arena.push_back(value);
            }
        }
        entries.insert_or_assign(Hash(start, parameters), offset);
    }

    /// Forgets every call when the macro code is no longer the code they ran.
    void Synchronize(std::uint64_t code_generation) {
        if (code_generation != generation) {
            generation = code_generation;
            arena.clear();
            entries.clear();
        }
    }

private:
    static constexpr std::size_t HeaderWords = 5;
    static constexpr std::size_t MaxArenaWords = std::size_t{1} << 22; // 16 MiB

    static std::uint64_t Hash(std::uint32_t start, std::span<const std::uint32_t> parameters) {
        std::uint64_t hash = 0x9e3779b97f4a7c15ULL ^ (std::uint64_t{start} << 32) ^ parameters.size();
        for (const std::uint32_t word : parameters) {
            hash = (hash ^ word) * 0xff51afd7ed558ccdULL;
            hash ^= hash >> 32;
        }
        return hash;
    }

    Map entries;
    std::vector<std::uint32_t> arena;
    std::uint64_t generation{};
};

} // namespace Tegra
