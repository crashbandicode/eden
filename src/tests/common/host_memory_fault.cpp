// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include "common/host_memory.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
// Ask the host VM, independently of Eden's page table and bookkeeping.
bool Inaccessible(const void* pointer) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(pointer, &info, sizeof(info)) != sizeof(info)) return false;
    return info.State == MEM_RESERVE ||
           (info.State == MEM_COMMIT && (info.Protect & PAGE_NOACCESS));
#elif defined(__linux__)
    std::ifstream maps("/proc/self/maps");
    std::string line;
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    while (std::getline(maps, line)) {
        std::istringstream fields(line);
        std::string range, permissions;
        fields >> range >> permissions;
        const auto split = range.find('-');
        const auto start = std::stoull(range.substr(0, split), nullptr, 16);
        const auto end = std::stoull(range.substr(split + 1), nullptr, 16);
        if (start <= address && address < end) return permissions.substr(0, 3) == "---";
    }
    return false;
#else
    return false;
#endif
}
}

TEST_CASE("Untouched fastmem reservation must fault, including low guest addresses", "[unmapped]") {
    Common::HostMemory memory(2 * 1024 * 1024, 64 * 1024 * 1024);
    auto* arena = memory.VirtualBasePointer();
    REQUIRE(arena != nullptr);
    CHECK(Inaccessible(arena + 0x21));
    CHECK(Inaccessible(arena + 0x123021));
    CHECK(Inaccessible(arena + 64 * 1024 * 1024 - 1));
    memory.Map(0x120000, 0x10000, 0x2000, Common::MemoryPermission::ReadWrite, false);
    auto* data = reinterpret_cast<volatile u8*>(arena + 0x120021);
    *data = 0x7b;
    REQUIRE(*data == 0x7b);
    REQUIRE(memory.BackingBasePointer()[0x10021] == 0x7b);
    CHECK(Inaccessible(arena + 0x21));
    CHECK(Inaccessible(arena + 0x122021));
    memory.Unmap(0x120000, 0x2000, false);
    CHECK(Inaccessible(arena + 0x120021));
    memory.Map(0x120000, 0x10000, 0x2000, Common::MemoryPermission::ReadWrite, false);
    CHECK(*data == 0x7b);
    CHECK(Inaccessible(arena + 0x21));
}
