// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Core {

// Observational module map. Call Register only after a successful guest mapping.
class TraceModules {
public:
    struct Module {
        std::string name;
        std::string build_id; // Hex; empty/all-zero means unavailable.
        u64 base{};
        u64 size{};
        u64 process_id{};
    };

    struct Description {
        Module module;
        u64 module_offset{};
        std::string symbol; // Empty when no containing symbol is known.
        u64 symbol_offset{};
        std::string text; // module!symbol+0xoffset or module+0xoffset
    };

    // Returns false for an invalid range/name or registry capacity exhaustion.
    bool Register(Module module);
    void Unregister(u64 process_id, u64 base);
    // Clears mappings AND supplied symbols; call on system shutdown, not after preboot loading.
    void Clear();
    [[nodiscard]] u64 Revision() const;
    [[nodiscard]] std::vector<Module> List(u64 process_id) const;
    [[nodiscard]] std::optional<u64> Resolve(u64 process_id, std::string_view target) const;
    // Empty string means success. Transactional: errors preserve previously loaded symbols.
    // A preboot name can alias an anonymous runtime NRO only through a matching nonzero build ID.
    std::string LoadSymbols(std::string_view module_name, const std::filesystem::path& elf_path);
    [[nodiscard]] std::optional<Description> Describe(u64 process_id, u64 address) const;

private:
    struct Symbol {
        std::string name;
        u64 offset{};
        u64 size{};
    };
    struct Symbols {
        std::string build_id;
        std::vector<Symbol> ordered;
        std::map<std::string, std::optional<u64>, std::less<>> by_name;
    };
    static std::string ParseElf(const std::vector<u8>& data, Symbols& symbols);
    const Module* FindModule(u64 process_id, std::string_view name) const;
    const Symbols* FindSymbols(const Module& module, std::string_view name) const;

    mutable std::shared_mutex mutex;
    std::vector<Module> modules;
    std::map<std::string, Symbols, std::less<>> symbol_sets;
    std::atomic<u64> revision{};
};

} // namespace Core
