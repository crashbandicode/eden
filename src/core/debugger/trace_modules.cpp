// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <openssl/evp.h>

#include "core/debugger/trace_modules.h"

namespace Core {
namespace {
constexpr u64 MaxElfBytes = 256ULL * 1024 * 1024;
constexpr u64 MaxSymbols = 250000;
constexpr u64 MaxNameBytes = 16 * 1024 * 1024;
constexpr size_t MaxModules = 4096;
constexpr size_t MaxSymbolSets = 16;

bool Range(u64 offset, u64 size, u64 total) {
    return offset <= total && size <= total - offset;
}

bool ValidName(std::string_view name) {
    return !name.empty() && name.size() <= 4096 &&
           name.find_first_of("!+\r\n") == std::string_view::npos &&
           name.find('\0') == std::string_view::npos;
}

std::string Hex(std::span<const u8> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

// Switch stores the ELF build ID in a zero-padded 32-byte field.
std::string NormalizeId(std::string id) {
    if (id.size() > 64 || id.size() % 2 != 0) {
        return {};
    }
    for (auto& c : id) {
        if (c >= 'A' && c <= 'F') {
            c += 'a' - 'A';
        }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return {};
        }
    }
    if (id.find_first_not_of('0') == std::string::npos) {
        return {};
    }
    id.resize(64, '0');
    return id;
}

bool Matches(std::string_view left, std::string_view right) {
    return !left.empty() && left == right;
}

std::string OffsetText(u64 offset) {
    char text[16];
    const auto result = std::to_chars(std::begin(text), std::end(text), offset, 16);
    return "+0x" + std::string(text, result.ptr);
}
} // namespace

bool TraceModules::Register(Module module) {
    if (!ValidName(module.name) || module.size == 0 ||
        module.size > std::numeric_limits<u64>::max() - module.base) {
        return false;
    }
    module.build_id = NormalizeId(std::move(module.build_id));
    std::unique_lock lock{mutex};
    const auto existing = std::ranges::find_if(modules, [&](const Module& entry) {
        return entry.process_id == module.process_id && entry.base == module.base;
    });
    if (existing != modules.end()) {
        *existing = std::move(module);
    } else {
        if (modules.size() >= MaxModules) {
            return false;
        }
        modules.push_back(std::move(module));
    }
    revision.fetch_add(1, std::memory_order_release);
    return true;
}

void TraceModules::Unregister(u64 process_id, u64 base) {
    std::unique_lock lock{mutex};
    std::erase_if(modules, [&](const Module& module) {
        return module.process_id == process_id && module.base == base;
    });
    revision.fetch_add(1, std::memory_order_release);
}

void TraceModules::Clear() {
    std::unique_lock lock{mutex};
    modules.clear();
    symbol_sets.clear();
    revision.fetch_add(1, std::memory_order_release);
}

u64 TraceModules::Revision() const {
    return revision.load(std::memory_order_acquire);
}

std::vector<TraceModules::Module> TraceModules::List(u64 process_id) const {
    std::shared_lock lock{mutex};
    std::vector<Module> result;
    for (const auto& module : modules) {
        if (module.process_id == process_id) {
            result.push_back(module);
        }
    }
    std::ranges::sort(result, {}, &Module::base);
    return result;
}

const TraceModules::Module* TraceModules::FindModule(u64 process_id, std::string_view name) const {
    const auto symbols = symbol_sets.find(name);
    const Module* result = nullptr;
    for (const auto& module : modules) {
        if (module.process_id != process_id) {
            continue;
        }
        const bool alias = module.name.starts_with("nro:") && symbols != symbol_sets.end() &&
                           Matches(module.build_id, symbols->second.build_id);
        if (module.name == name || alias) {
            if (result) {
                return nullptr; // Never silently select one of two mappings.
            }
            result = &module;
        }
    }
    return result;
}

const TraceModules::Symbols* TraceModules::FindSymbols(const Module& module,
                                                      std::string_view name) const {
    const auto it = symbol_sets.find(name);
    if (it == symbol_sets.end()) {
        return nullptr;
    }
    const auto& symbols = it->second;
    if (!module.build_id.empty() && !symbols.build_id.empty() &&
        module.build_id != symbols.build_id) {
        return nullptr;
    }
    return &symbols;
}

std::optional<u64> TraceModules::Resolve(u64 process_id, std::string_view target) const {
    const auto separator = target.find_first_of("+!");
    if (separator == std::string_view::npos || separator == 0) {
        return std::nullopt;
    }
    const auto name = target.substr(0, separator);
    auto value = target.substr(separator + 1);
    std::shared_lock lock{mutex};
    const auto* module = FindModule(process_id, name);
    if (!module) {
        return std::nullopt;
    }
    u64 offset{};
    if (target[separator] == '+') {
        int radix = 10;
        if (value.starts_with("0x") || value.starts_with("0X")) {
            radix = 16;
            value.remove_prefix(2);
        }
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), offset, radix);
        if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            return std::nullopt;
        }
    } else {
        const auto* symbols = FindSymbols(*module, name);
        if (!symbols) {
            return std::nullopt;
        }
        const auto symbol = symbols->by_name.find(value);
        if (symbol == symbols->by_name.end() || !symbol->second) {
            return std::nullopt;
        }
        offset = *symbol->second;
    }
    if (offset >= module->size) {
        return std::nullopt;
    }
    return module->base + offset;
}

std::string TraceModules::ParseElf(const std::vector<u8>& data, Symbols& symbols) {
    const auto read = [&](u64 offset, unsigned size) {
        u64 value{};
        for (unsigned i = 0; i < size; ++i) {
            value |= static_cast<u64>(data[offset + i]) << (8 * i);
        }
        return value;
    };
    if (data.size() < 64 || read(0, 4) != 0x464c457f || data[4] != 2 || data[5] != 1 ||
        data[6] != 1 || read(18, 2) != 183 || read(20, 4) != 1 || read(52, 2) != 64 ||
        (read(16, 2) != 2 && read(16, 2) != 3)) {
        return "unsupported_elf: expected ELF64 little-endian AArch64 EXEC/DYN";
    }
    const u64 phoff = read(32, 8), shoff = read(40, 8);
    const u64 phnum = read(56, 2), shnum = read(60, 2);
    if (phnum == 0 || phnum > 1024 || read(54, 2) != 56 ||
        !Range(phoff, phnum * 56, data.size()) || shnum == 0 || shnum > 4096 ||
        read(58, 2) != 64 || !Range(shoff, shnum * 64, data.size())) {
        return "invalid_elf: missing or invalid program/section table";
    }
    const auto parse_notes = [&](u64 offset, u64 size) -> std::string {
        if (!Range(offset, size, data.size())) {
            return "invalid_elf: note outside file";
        }
        u64 pos = offset;
        const u64 end = offset + size;
        while (pos < end) {
            if (!Range(pos, 12, end)) {
                return "invalid_elf: truncated note";
            }
            const auto namesz = read(pos, 4), descsz = read(pos + 4, 4), kind = read(pos + 8, 4);
            pos += 12;
            const auto aligned_name = (namesz + 3) & ~u64{3};
            const auto aligned_desc = (descsz + 3) & ~u64{3};
            if (!Range(pos, aligned_name, end) || !Range(pos + aligned_name, aligned_desc, end)) {
                return "invalid_elf: note outside section";
            }
            if (kind == 3 && namesz == 4 && read(pos, 4) == 0x00554e47) {
                if (descsz == 0 || descsz > 32) {
                    return "unsupported_elf: build ID must fit Switch's 32-byte field";
                }
                auto id = NormalizeId(Hex(std::span{data}.subspan(pos + aligned_name, descsz)));
                if (!symbols.build_id.empty() && symbols.build_id != id) {
                    return "invalid_elf: conflicting build IDs";
                }
                symbols.build_id = std::move(id);
            }
            pos += aligned_name + aligned_desc;
        }
        return {};
    };
    u64 image_base = std::numeric_limits<u64>::max();
    for (u64 i = 0; i < phnum; ++i) {
        const auto ph = phoff + i * 56;
        const auto type = read(ph, 4);
        const auto offset = read(ph + 8, 8), address = read(ph + 16, 8);
        const auto file_size = read(ph + 32, 8), memory_size = read(ph + 40, 8);
        if (type == 4) { // PT_NOTE
            if (auto error = parse_notes(offset, file_size); !error.empty()) {
                return error;
            }
        }
        if (type != 1) {
            continue;
        }
        if (!Range(offset, file_size, data.size()) || file_size > memory_size ||
            memory_size > std::numeric_limits<u64>::max() - address) {
            return "invalid_elf: invalid load segment";
        }
        image_base = std::min(image_base, address);
    }
    if (image_base == std::numeric_limits<u64>::max()) {
        return "invalid_elf: no load segment";
    }
    u64 symbol_count{}, name_bytes{};
    for (u64 i = 0; i < shnum; ++i) {
        const auto sh = shoff + i * 64;
        const auto type = read(sh + 4, 4), offset = read(sh + 24, 8), size = read(sh + 32, 8);
        if (type != 8 && !Range(offset, size, data.size())) {
            return "invalid_elf: section outside file";
        }
        if (type == 7) { // SHT_NOTE
            if (auto error = parse_notes(offset, size); !error.empty()) {
                return error;
            }
        }
        if (type != 2 && type != 11) { // SHT_SYMTAB / SHT_DYNSYM
            continue;
        }
        const auto link = read(sh + 40, 4);
        if (read(sh + 56, 8) != 24 || size % 24 != 0 || link >= shnum ||
            size / 24 > MaxSymbols - symbol_count) {
            return "invalid_elf: invalid or excessive symbol table";
        }
        symbol_count += size / 24;
        const auto strings = shoff + link * 64;
        const auto str_offset = read(strings + 24, 8), str_size = read(strings + 32, 8);
        if (read(strings + 4, 4) != 3 || !Range(str_offset, str_size, data.size())) {
            return "invalid_elf: invalid symbol string table";
        }
        for (u64 pos = offset; pos < offset + size; pos += 24) {
            const auto name = read(pos, 4), section = read(pos + 6, 2);
            const auto value = read(pos + 8, 8), symbol_size = read(pos + 16, 8);
            const auto kind = data[pos + 4] & 15;
            // Defined ordinary address symbols only; TLS, ABS, COMMON and undefined aren't relocatable.
            if (section == 0 || section >= 0xff00 || (kind != 0 && kind != 1 && kind != 2)) {
                continue;
            }
            if (section >= shnum || name >= str_size) {
                return "invalid_elf: invalid symbol section/name";
            }
            const auto target_section = shoff + section * 64;
            if ((read(target_section + 8, 8) & 2) == 0) { // SHF_ALLOC
                continue;
            }
            const auto section_address = read(target_section + 16, 8);
            const auto section_size = read(target_section + 32, 8);
            if (value < image_base || value < section_address ||
                !Range(value - section_address, symbol_size, section_size) ||
                symbol_size > std::numeric_limits<u64>::max() - value) {
                return "invalid_elf: symbol outside section";
            }
            const auto start = data.begin() + str_offset + name;
            const auto limit = start + std::min<u64>(str_size - name, 4097);
            const auto end = std::find(start, limit, 0);
            if (end == limit) {
                return "invalid_elf: unterminated or oversized symbol name";
            }
            if (start == end) {
                continue;
            }
            std::string symbol_name(start, end);
            name_bytes += symbol_name.size();
            if (name_bytes > MaxNameBytes) {
                return "invalid_elf: excessive symbol names";
            }
            const auto relative = value - image_base;
            auto [entry, inserted] = symbols.by_name.emplace(symbol_name, relative);
            if (!inserted && entry->second != relative) {
                entry->second.reset(); // Duplicate names at distinct addresses are ambiguous.
            }
            symbols.ordered.push_back({std::move(symbol_name), relative, symbol_size});
        }
    }
    if (symbols.ordered.empty()) {
        return "invalid_elf: no defined address symbols";
    }
    // linkle's NRO converter falls back to SHA-256 of the page-padded ELF load
    // segments, BEFORE it replaces the NRO and MOD0 headers. This is not a hash
    // of the ELF file or the final NRO. Recognize only its zero-based RX/R/RW
    // layout; other note-less ELFs retain explicit-name symbol loading only.
    if (symbols.build_id.empty() && phnum >= 3 && image_base == 0) {
        std::array<u64, 3> offsets{}, sizes{}, padded{};
        bool compatible = true;
        u64 total{};
        for (size_t i = 0; i < 3; ++i) {
            const auto ph = phoff + i * 56;
            const auto address = read(ph + 16, 8);
            offsets[i] = read(ph + 8, 8);
            sizes[i] = read(ph + 32, 8);
            if (read(ph, 4) != 1 || read(ph + 4, 4) != std::array<u64, 3>{5, 4, 6}[i] ||
                address != total || sizes[i] > MaxElfBytes) {
                compatible = false;
                break;
            }
            padded[i] = (sizes[i] + 0xfff) & ~u64{0xfff};
            if (i < 2 || (phnum > 3 && read(ph + 56, 4) == 1)) {
                const auto next = read(ph + 56 + 16, 8);
                if (next < total || next - total < padded[i] || next % 0x1000 != 0) {
                    compatible = false;
                    break;
                }
                padded[i] = next - total;
            }
            if (padded[i] > MaxElfBytes - total) {
                compatible = false;
                break;
            }
            total += padded[i];
        }
        // A fourth PT_LOAD may describe BSS, never additional file bytes.
        for (u64 i = 3; compatible && i < phnum; ++i) {
            const auto ph = phoff + i * 56;
            if (read(ph, 4) == 1 &&
                (i != 3 || read(ph + 32, 8) != 0 || read(ph + 16, 8) != total)) {
                compatible = false;
            }
        }
        if (compatible) {
            std::vector<u8> image(static_cast<size_t>(total), 0);
            size_t position{};
            for (size_t i = 0; i < 3; ++i) {
                std::copy_n(data.begin() + offsets[i], sizes[i], image.begin() + position);
                position += padded[i];
            }
            std::array<u8, 32> digest{};
            unsigned length{};
            if (EVP_Digest(image.data(), image.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
                length != digest.size()) {
                return "elf_hash_failed";
            }
            symbols.build_id = Hex(digest);
        }
    }
    std::ranges::sort(symbols.ordered, [](const Symbol& a, const Symbol& b) {
        return a.offset != b.offset ? a.offset < b.offset : a.name < b.name;
    });
    return {};
}

std::string TraceModules::LoadSymbols(std::string_view module_name,
                                      const std::filesystem::path& elf_path) {
    if (!ValidName(module_name)) {
        return "invalid_module_name";
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(elf_path, error)) {
        return "elf_not_regular_file";
    }
    std::ifstream file(elf_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return "elf_open_failed";
    }
    const auto length = file.tellg();
    if (length < 64 || length > static_cast<std::streamoff>(MaxElfBytes)) {
        return "elf_size_out_of_bounds";
    }
    std::vector<u8> bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return "elf_read_failed";
    }
    Symbols symbols;
    if (auto result = ParseElf(bytes, symbols); !result.empty()) {
        return result;
    }
    std::unique_lock lock{mutex};
    size_t matches{};
    for (const auto& module : modules) {
        if (module.name == module_name ||
            (module.name.starts_with("nro:") && Matches(module.build_id, symbols.build_id))) {
            if (++matches > 1) {
                return "ambiguous_module";
            }
            if (!module.build_id.empty() && !symbols.build_id.empty() &&
                module.build_id != symbols.build_id) {
                return "build_id_mismatch";
            }
        }
    }
    if (!symbol_sets.contains(module_name) && symbol_sets.size() >= MaxSymbolSets) {
        return "symbol_capacity_exceeded";
    }
    symbol_sets.insert_or_assign(std::string(module_name), std::move(symbols));
    revision.fetch_add(1, std::memory_order_release);
    return {};
}

std::optional<TraceModules::Description> TraceModules::Describe(u64 process_id, u64 address) const {
    std::shared_lock lock{mutex};
    const Module* found = nullptr;
    for (const auto& module : modules) {
        if (module.process_id == process_id && address >= module.base && address - module.base < module.size) {
            if (found) {
                return std::nullopt;
            }
            found = &module;
        }
    }
    if (!found) {
        return std::nullopt;
    }
    Description result{*found, address - found->base, {}, 0, {}};
    result.text = found->name + OffsetText(result.module_offset);
    const auto* symbols = FindSymbols(*found, found->name);
    if (!symbols && found->name.starts_with("nro:")) {
        for (const auto& [name, candidate] : symbol_sets) {
            if (Matches(found->build_id, candidate.build_id)) {
                if (symbols) {
                    return result; // Multiple user aliases: retain factual module attribution.
                }
                symbols = &candidate;
            }
        }
    }
    if (!symbols) {
        return result;
    }
    auto it = std::upper_bound(symbols->ordered.begin(), symbols->ordered.end(), result.module_offset,
                               [](u64 offset, const Symbol& symbol) { return offset < symbol.offset; });
    if (it != symbols->ordered.begin()) {
        --it;
        const auto delta = result.module_offset - it->offset;
        if (delta == 0 || delta < it->size) {
            result.symbol = it->name;
            result.symbol_offset = delta;
            result.text = found->name + "!" + it->name + OffsetText(delta);
        }
    }
    return result;
}

} // namespace Core
