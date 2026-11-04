// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_location>
#include <thread>
#include <vector>

#include "core/debugger/trace_modules.h"

namespace {
void Require(bool condition, const std::source_location where = std::source_location::current()) {
    if (!condition) {
        std::cerr << "FAIL " << where.file_name() << ':' << where.line() << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void Put(std::vector<u8>& data, size_t offset, u64 value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        data.at(offset + i) = static_cast<u8>(value >> (i * 8));
    }
}

// Independent fixture layout: ELF linked at 0x400000; two text symbols at +0x110/+0x140.
std::vector<u8> Elf(bool dynamic_symbols = false) {
    std::vector<u8> data(0x500);
    Put(data, 0, 0x464c457f, 4);
    data[4] = 2;
    data[5] = 1;
    data[6] = 1;
    Put(data, 16, 3, 2);
    Put(data, 18, 183, 2);
    Put(data, 20, 1, 4);
    Put(data, 32, 0x40, 8);
    Put(data, 40, 0x300, 8);
    Put(data, 52, 64, 2);
    Put(data, 54, 56, 2);
    Put(data, 56, 1, 2);
    Put(data, 58, 64, 2);
    Put(data, 60, 5, 2);
    Put(data, 0x40, 1, 4); // PT_LOAD
    Put(data, 0x50, 0x400000, 8);
    Put(data, 0x60, data.size(), 8);
    Put(data, 0x68, data.size(), 8);

    const auto section = [&](size_t index, u32 type, u64 flags, u64 addr, u64 offset, u64 size) {
        const size_t sh = 0x300 + index * 64;
        Put(data, sh + 4, type, 4);
        Put(data, sh + 8, flags, 8);
        Put(data, sh + 16, addr, 8);
        Put(data, sh + 24, offset, 8);
        Put(data, sh + 32, size, 8);
    };
    section(1, 1, 6, 0x400100, 0x100, 0x80);
    section(2, 3, 0, 0, 0x200, 9);
    section(3, dynamic_symbols ? 11 : 2, 0, 0, 0x240, 72);
    section(4, 7, 0, 0, 0x290, 20);
    Put(data, 0x3c0 + 40, 2, 4); // symtab -> strtab
    Put(data, 0x3c0 + 56, 24, 8);
    const char names[] = "\0foo\0bar";
    std::copy(std::begin(names), std::end(names), data.begin() + 0x200);
    Put(data, 0x258, 1, 4);
    data[0x25c] = 0x12; // STB_GLOBAL, STT_FUNC
    Put(data, 0x25e, 1, 2);
    Put(data, 0x260, 0x400110, 8);
    Put(data, 0x268, 0x10, 8);
    Put(data, 0x270, 5, 4);
    data[0x274] = 0x12;
    Put(data, 0x276, 1, 2);
    Put(data, 0x278, 0x400140, 8);
    Put(data, 0x280, 8, 8);
    Put(data, 0x290, 4, 4);
    Put(data, 0x294, 4, 4);
    Put(data, 0x298, 3, 4); // NT_GNU_BUILD_ID
    Put(data, 0x29c, 0x00554e47, 4);
    Put(data, 0x2a0, 0x78563412, 4);
    return data;
}

struct FixtureFile {
    std::filesystem::path path;
    FixtureFile() {
        const auto root = std::filesystem::is_directory("/tmp/opencode")
                              ? std::filesystem::path{"/tmp/opencode"}
                              : std::filesystem::temp_directory_path();
        path = root / ("eden-trace-modules-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()) + ".elf");
    }
    ~FixtureFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    void Write(const std::vector<u8>& bytes) const {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        Require(static_cast<bool>(file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
    }
};

// Original note-less, zero-based ELF. Independently hashed image bytes:
// 384 cycling bytes, padding to 8192; "abc", padding to 8192; "12345",
// padding to 8192 (separate BSS) or 4096 (embedded BSS).
std::vector<u8> LinkleElf(bool separate_bss) {
    auto data = Elf();
    data.resize(0x3100);
    Put(data, 32, 0x500, 8);
    Put(data, 56, separate_bss ? 4 : 3, 2);
    Put(data, 60, 4, 2); // No note section.
    Put(data, 0x350, 0x100, 8); // Allocated text section address.
    Put(data, 0x358, 0x1100, 8);
    Put(data, 0x260, 0x110, 8);
    Put(data, 0x278, 0x140, 8);
    const auto segment = [&](size_t index, u32 flags, u64 offset, u64 address, u64 size, u64 memory) {
        const auto ph = 0x500 + index * 56;
        Put(data, ph, 1, 4);
        Put(data, ph + 4, flags, 4);
        Put(data, ph + 8, offset, 8);
        Put(data, ph + 16, address, 8);
        Put(data, ph + 32, size, 8);
        Put(data, ph + 40, memory, 8);
        Put(data, ph + 48, 0x1000, 8);
    };
    segment(0, 5, 0x1000, 0, 384, 384);
    segment(1, 4, 0x2000, 8192, 3, 3);
    segment(2, 6, 0x3000, 16384, 5, separate_bss ? 5 : 8192);
    if (separate_bss) segment(3, 6, 0x3100, 24576, 0, 4096);
    for (size_t i = 0; i < 384; ++i) data[0x1000 + i] = static_cast<u8>(i % 251);
    std::copy_n("abc", 3, data.begin() + 0x2000);
    std::copy_n("12345", 5, data.begin() + 0x3000);
    return data;
}
} // namespace

int main(int argc, char** argv) {
    using Registry = Core::TraceModules;
    constexpr u64 Max = std::numeric_limits<u64>::max();
    FixtureFile fixture;
    if (argc == 3 && std::string_view{argv[1]} == "--export-linkle") {
        for (bool bss : {true, false}) {
            const auto bytes = LinkleElf(bss);
            std::ofstream out(std::filesystem::path{argv[2]} / (bss ? "bss.elf" : "embedded.elf"), std::ios::binary);
            Require(static_cast<bool>(out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
        }
        return 0;
    }
    if (argc == 5 && std::string_view{argv[1]} == "--check-elf") {
        Registry registry;
        Require(registry.LoadSymbols("release.nro", argv[2]).empty());
        Require(registry.Register({"nro:" + std::string{argv[3]}, argv[3], 0xa00000, 0x1000000, 9}));
        Require(registry.Resolve(9, "release.nro!" + std::string{argv[4]}).has_value());
        std::cout << "PASS retained ELF identity alias and named symbol\n";
        return 0;
    }
    Require(argc == 1);
    for (bool bss : {true, false}) {
        Registry registry;
        const std::string id = bss ? "b0b6ae7d81429d4c6a7b213e8e13b266652cfa4e202169324374700f33975bb5"
                                   : "039676beaf1e26426fdd2746b512529a7da8802f01546fe4f157181d09e56a34";
        auto bytes = LinkleElf(bss);
        fixture.Write(bytes);
        Require(registry.LoadSymbols("plugin.nro", fixture.path).empty());
        Require(registry.Register({"nro:" + id, id, 0xa00000, 0x10000, 9}));
        Require(registry.Resolve(9, "plugin.nro!foo") == u64{0xa00110});
        Require(registry.Describe(9, 0xa00115)->symbol == "foo");
        registry.Unregister(9, 0xa00000);
        Require(!registry.Resolve(9, "plugin.nro!foo"));
        Require(registry.Register({"nro:" + id, id, 0xb00000, 0x10000, 9}));
        Require(registry.Resolve(9, "plugin.nro!foo") == u64{0xb00110});
        // Content mismatch cannot retain a stale filename alias.
        bytes[0x1000] ^= 1;
        fixture.Write(bytes);
        Require(registry.LoadSymbols("plugin.nro", fixture.path).empty());
        Require(!registry.Resolve(9, "plugin.nro!foo"));
        // Unbounded virtual gaps must not allocate or synthesize an identity.
        bytes = LinkleElf(bss);
        Put(bytes, 0x500 + 56 + 16, u64{1} << 40, 8);
        fixture.Write(bytes);
        Require(registry.LoadSymbols("plugin.nro", fixture.path).empty());
        Require(!registry.Resolve(9, "plugin.nro!foo"));
        // Invalid segment bytes are rejected transactionally.
        bytes = LinkleElf(bss);
        Put(bytes, 0x500 + 8, Max - 1, 8);
        fixture.Write(bytes);
        const auto revision = registry.Revision();
        Require(!registry.LoadSymbols("plugin.nro", fixture.path).empty());
        Require(registry.Revision() == revision);
    }
    std::cout << "PASS note-less linkle identity, BSS variants, content mismatch, relocation and bounded gaps\n";
    {
        Registry registry;
        Require(registry.Revision() == 0);
        Require(registry.Register({"main", "12345678", 0x800000, 0x500, 7}));
        Require(registry.Revision() == 1);
        Require(registry.Resolve(7, "main+0x110") == u64{0x800110});
        Require(registry.Resolve(7, "main+272") == u64{0x800110});
        Require(!registry.Resolve(8, "main+0x110"));
        for (const auto* bad : {"main+0x500", "main+-1", "main+0x10000000000000000", "main+",
                                "main+0x", "main+10x"}) {
            Require(!registry.Resolve(7, bad));
        }
        Require(!registry.Register({"overflow", {}, Max - 8, 16, 7}));
        Require(!registry.Register({"empty", {}, 0x1000, 0, 7}));
        Require(registry.Register({"near-end", {}, Max - 0x100, 0x100, 7}));
        Require(registry.Resolve(7, "near-end+255") == Max - 1);
        registry.Unregister(7, Max - 0x100);
        Require(registry.List(7).size() == 1);
        Require(registry.List(8).empty());
    }
    std::cout << "PASS module ranges, process isolation, numeric syntax and overflow\n";
    {
        Registry registry;
        fixture.Write(Elf());
        Require(registry.LoadSymbols("main", fixture.path).empty());
        Require(registry.Revision() == 1);
        Require(!registry.Resolve(7, "main!foo"));
        Require(registry.Register({"main", "12345678", 0x800000, 0x500, 7}));
        Require(registry.Resolve(7, "main!foo") == u64{0x800110});
        Require(registry.Resolve(7, "main!bar") == u64{0x800140});
        const auto description = registry.Describe(7, 0x800115);
        Require(description && description->symbol == "foo" && description->symbol_offset == 5);
        Require(description->text == "main!foo+0x5" && description->module_offset == 0x115);
        Require(registry.Describe(7, 0x800130)->symbol.empty());
        Require(!registry.Describe(7, 0x800500));
        registry.Unregister(7, 0x800000);
        Require(!registry.Resolve(7, "main!foo"));
        Require(!registry.Describe(7, 0x800110));
        Require(registry.Register({"main", "12345678", 0x900000, 0x500, 7}));
        Require(registry.Resolve(7, "main!foo") == u64{0x900110});
        fixture.Write(Elf(true));
        Require(registry.LoadSymbols("main", fixture.path).empty());
        Require(registry.Resolve(7, "main!bar") == u64{0x900140});
        const auto revision = registry.Revision();
        registry.Clear();
        Require(registry.Revision() == revision + 1 && registry.List(7).empty());
        Require(registry.Register({"main", "12345678", 0x900000, 0x500, 7}));
        Require(!registry.Resolve(7, "main!foo"));
    }
    std::cout << "PASS preboot symtab/dynsym, relocation, description, unload and Clear\n";
    {
        Registry registry;
        fixture.Write(Elf());
        Require(registry.LoadSymbols("plugin.nro", fixture.path).empty());
        Require(registry.Register({"nro:12345678", "12345678", 0xa00000, 0x500, 9}));
        Require(registry.Resolve(9, "plugin.nro!foo") == u64{0xa00110});
        Require(registry.Resolve(9, "plugin.nro+0x140") == u64{0xa00140});
        Require(registry.List(9).front().name == "nro:12345678"); // Do not fabricate a guest filename.
        Require(registry.Describe(9, 0xa00110)->symbol == "foo");
        Require(registry.Register({"nro:12345678", "12345678", 0xb00000, 0x500, 9}));
        Require(!registry.Resolve(9, "plugin.nro!foo"));
        Require(registry.LoadSymbols("plugin.nro", fixture.path) == "ambiguous_module");
        registry.Unregister(9, 0xa00000);
        Require(registry.Resolve(9, "plugin.nro!foo") == u64{0xb00110});
    }
    std::cout << "PASS factual runtime identity, build-ID alias and ambiguous mapping refusal\n";
    {
        Registry registry;
        fixture.Write(Elf());
        Require(registry.Register({"main", "abcdef00", 0x800000, 0x500, 7}));
        Require(registry.Register({"nro:12345678", "12345678", 0xa00000, 0x500, 7}));
        Require(registry.Register({"nro:9abc0000", "9abc0000", 0xc00000, 0x500, 7}));
        using Names = std::vector<std::string>;
        const auto selects = [&](u64 address, const Names& names, u64 process = 7) {
            return registry.Selects(process, address, names);
        };
        Require(selects(0x800010, {"main"}) && !selects(0x800010, {"nro:*"}));
        Require(selects(0xa00010, {"nro:*"}) && selects(0xc004ff, {"nro:*"}) && !selects(0xa00010, {"main"}));
        Require(selects(0xa00010, {"nro:12345678"}) && !selects(0xc00010, {"nro:12345678"}));
        Require(!selects(0xa00010, {"plugin.nro"})); // No alias until its ELF is loaded.
        Require(registry.LoadSymbols("plugin.nro", fixture.path).empty());
        Require(selects(0xa00010, {"plugin.nro"}) && !selects(0xc00010, {"plugin.nro"}));
        Require(!selects(0x800010, {"plugin.nro"})); // Aliases name runtime NROs only.
        Require(selects(0xc00010, {"main", "nro:9abc0000"}));
        Require(!selects(0xa00500, {"nro:*"}) && !selects(0x21, {"nro:*", "main"}));
        Require(!selects(0xa00010, {"nro:*"}, 8)); // Process isolation.
        Require(!selects(0xa00010, {}));
        Require(registry.Register({"overlap", {}, 0xa00400, 0x200, 7}));
        Require(!selects(0xa00450, {"nro:*"}) && !selects(0xa00450, {"overlap"}));
        Require(Registry::ValidSelector("nro:*") && Registry::ValidSelector("libtraining_rooms.nro"));
        for (const auto* bad : {"", "main+0x10", "a!b", "line\nbreak"}) Require(!Registry::ValidSelector(bad));
    }
    std::cout << "PASS fault module selection: names, nro:* wildcard, loaded aliases, outside/overlap refusal\n";
    {
        Registry registry;
        fixture.Write(Elf());
        Require(registry.Register({"main", "12345678", 0x800000, 0x500, 7}));
        Require(registry.LoadSymbols("main", fixture.path).empty());
        auto wrong = Elf();
        wrong[0x2a0] = 0xff;
        fixture.Write(wrong);
        const auto revision = registry.Revision();
        Require(registry.LoadSymbols("main", fixture.path) == "build_id_mismatch");
        Require(registry.Revision() == revision);
        Require(registry.Resolve(7, "main!foo") == u64{0x800110});
        auto program_note = Elf();
        Put(program_note, 56, 2, 2);
        Put(program_note, 0x78, 4, 4); // PT_NOTE; no SHT_NOTE remains.
        Put(program_note, 0x80, 0x290, 8);
        Put(program_note, 0x98, 20, 8);
        Put(program_note, 0x400 + 4, 1, 4);
        fixture.Write(program_note);
        Require(registry.LoadSymbols("main", fixture.path).empty());
        program_note[0x2a0] = 0xff;
        fixture.Write(program_note);
        Require(registry.LoadSymbols("main", fixture.path) == "build_id_mismatch");
        // A mismatch discovered only when the module subsequently arrives must also stay unresolved.
        Require(registry.Register({"main", "abcdef00", 0x800000, 0x500, 7}));
        Require(!registry.Resolve(7, "main!foo"));
        Require(registry.Describe(7, 0x800110)->symbol.empty());
        fixture.Write(Elf());
        Require(registry.Register({"main", "12345678", 0x800000, 0x500, 7}));
        Require(registry.Register({"main", "12345678", 0x900000, 0x500, 8}));
        Require(registry.LoadSymbols("main", fixture.path) == "ambiguous_module");
    }
    std::cout << "PASS matching build ID, transactional load failure and cross-process ambiguity\n";
    {
        Registry registry;
        const auto malformed = [&](std::vector<u8> data) {
            fixture.Write(data);
            Require(!registry.LoadSymbols("bad", fixture.path).empty());
        };
        auto data = Elf();
        data[5] = 2;
        malformed(data);
        data = Elf();
        Put(data, 18, 62, 2);
        malformed(data);
        data = Elf();
        Put(data, 32, Max - 8, 8);
        malformed(data);
        data = Elf();
        Put(data, 0x380 + 32, Max, 8);
        malformed(data);
        data = Elf();
        Put(data, 0x294, 0xffffffff, 4);
        malformed(data);
        data = Elf();
        Put(data, 0x268, Max, 8);
        malformed(data);
        data = Elf();
        Put(data, 0x3c0 + 40, 6, 4);
        malformed(data);
        data = Elf();
        data[0x208] = 'x'; // bar lacks a terminator within its string table.
        malformed(data);
        data = Elf();
        data.resize(63);
        malformed(data);
        Require(registry.Revision() == 0);
    }
    std::cout << "PASS malformed headers, tables, build-ID notes, strings and symbol overflow\n";
    {
        Registry registry;
        auto data = Elf();
        Put(data, 0x270, 1, 4); // Two distinct definitions named foo.
        fixture.Write(data);
        Require(registry.LoadSymbols("main", fixture.path).empty());
        Require(registry.Register({"main", "12345678", 0x800000, 0x500, 7}));
        Require(!registry.Resolve(7, "main!foo"));
    }
    std::cout << "PASS duplicate symbol names cannot select an arbitrary function\n";
    {
        Registry registry;
        std::thread writer([&] {
            for (unsigned i = 0; i < 1000; ++i) {
                Require(registry.Register({"main", {}, 0x800000, 0x500, 7}));
                registry.Unregister(7, 0x800000);
            }
        });
        for (unsigned i = 0; i < 1000; ++i) {
            const auto resolved = registry.Resolve(7, "main+0x110");
            Require(!resolved || *resolved == 0x800110);
            Require(registry.List(7).size() <= 1);
        }
        writer.join();
        Require(registry.List(7).empty() && registry.Revision() == 2000);
    }
    std::cout << "PASS concurrent publication and lookup\n";
}
