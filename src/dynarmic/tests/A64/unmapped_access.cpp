// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "common/host_memory.h"
#include "dynarmic/tests/A64/testenv.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/halt_reason.h"

namespace {
struct FaultEnv : A64TestEnv {
    Dynarmic::A64::Jit* jit{};
    bool fault{};
    bool abort_reads{true};
    u64 count{}, address{}, pc{}, width{};
    bool write{};
    void Access(u64 addr, size_t size, bool store) {
        ++count;
        address = addr;
        width = size;
        pc = jit->GetPC();
        write = store;
        if (fault && (store || abort_reads)) jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
    }
    u64 MemoryRead(u64 addr, size_t size) override { Access(addr, size, false); return 0; }
    Vector MemoryRead128(u64 addr) override { Access(addr, 16, false); return {}; }
    void MemoryWrite(u64 addr, u64, size_t size) override { Access(addr, size, true); }
    void MemoryWrite128(u64 addr, Vector) override { Access(addr, 16, true); }
};
}

TEST_CASE("A64 unmapped callbacks retain the exact instruction and abort state", "[unmapped]") {
    // Independent architectural encodings: LDR/STR {B,H,W,X,Q} [X0], then ADD X9,X9,#1.
    constexpr std::array<u32, 10> instructions{
        0x39400008, 0x79400008, 0xb9400008, 0xf9400008, 0x3dc00008,
        0x39000008, 0x79000008, 0xb9000008, 0xf9000008, 0x3d800008};
    constexpr std::array<size_t, 5> sizes{1, 2, 4, 8, 16};
    Common::HostMemory memory(2 * 1024 * 1024, 64 * 1024 * 1024);
    REQUIRE(memory.VirtualBasePointer() != nullptr);
    std::array<void*, 16384> pages{};
    // Execute each access through callbacks, empty page table and real host-fault fastmem.
    for (int path = 0; path < 3; ++path) {
        for (bool fault : {false, true}) {
            for (size_t i = 0; i < instructions.size(); ++i) {
                for (u64 address : {u64{0x21}, u64{0xfff}, u64{1} << 40}) {
                    INFO("path=" << path << " fault=" << fault << " instruction=" << i << " address=" << address);
                    FaultEnv env;
                    env.fault = fault;
                    env.code_mem_start_address = 0x10000;
                    env.code_mem = {0xd2800aa5, instructions[i], 0x91000529, 0x14000000}; // MOV X5,#0x55
                    Dynarmic::A64::UserConfig conf;
                    conf.callbacks = &env;
                    conf.code_cache_size = 16 * 1024 * 1024;
                    conf.precise_memory_callback_pc = true;
                    conf.check_halt_on_memory_access = fault;
                    conf.page_table_address_space_bits = 26;
                    conf.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
                    conf.only_detect_misalignment_via_page_table_on_page_boundary = true;
                    if (path == 1) conf.page_table = pages.data();
                    if (path == 2) {
                        conf.fastmem_pointer = reinterpret_cast<uintptr_t>(memory.VirtualBasePointer());
                        conf.fastmem_address_space_bits = 26;
                        conf.silently_mirror_fastmem = false;
                    }
                    Dynarmic::A64::Jit jit{conf};
                    env.jit = &jit;
                    jit.SetPC(0x10000);
                    jit.SetRegister(0, address);
                    jit.SetRegister(8, 0xfeed);
                    jit.SetRegister(9, 7);
                    jit.SetVector(8, {0xfeed, 0xbeef});
                    env.ticks_left = 4;
                    const auto reason = jit.Run();
                    REQUIRE(env.count == 1);
                    CHECK(env.pc == 0x10004);
                    CHECK(env.address == address);
                    CHECK(env.width == sizes[i % 5]);
                    CHECK(env.write == (i >= 5));
                    CHECK(jit.GetRegister(0) == address);
                    CHECK(jit.GetRegister(5) == 0x55);
                    if (fault) {
                        CHECK(Dynarmic::Has(reason, Dynarmic::HaltReason::MemoryAbort));
                        CHECK(jit.GetPC() == 0x10004);
                        CHECK(jit.GetRegister(8) == 0xfeed);
                        CHECK(jit.GetRegister(9) == 7);
                        CHECK(jit.GetVector(8) == Vector{0xfeed, 0xbeef});
                    } else {
                        CHECK(jit.GetRegister(9) == 8);
                    }
                }
            }
        }
    }
}

TEST_CASE("A64 unmapped abort preserves base writeback and the original null-load destination", "[unmapped]") {
    // LDR X8,[X0,#32]; LDR X8,[X0],#8; LDR X8,[X0,#8]!; STR X8,[X0],#8.
    for (u32 instruction : {0xf9401008U, 0xf8408408U, 0xf8408c08U, 0xf8008408U}) {
        FaultEnv env;
        env.fault = true;
        env.code_mem_start_address = 0x20000;
        env.code_mem = {instruction, 0x91000529, 0x14000000};
        Dynarmic::A64::UserConfig conf;
        conf.callbacks = &env;
        conf.precise_memory_callback_pc = true;
        conf.check_halt_on_memory_access = true;
        conf.code_cache_size = 16 * 1024 * 1024;
        Dynarmic::A64::Jit jit{conf};
        env.jit = &jit;
        jit.SetPC(0x20000);
        jit.SetRegister(0, 1);
        jit.SetRegister(8, 0xfeed);
        jit.SetRegister(9, 7);
        env.ticks_left = 3;
        CHECK(Dynarmic::Has(jit.Run(), Dynarmic::HaltReason::MemoryAbort));
        CHECK(env.count == 1);
        CHECK(env.pc == 0x20000);
        CHECK(jit.GetPC() == 0x20000);
        CHECK(jit.GetRegister(0) == 1);
        CHECK(jit.GetRegister(8) == 0xfeed);
        CHECK(jit.GetRegister(9) == 7);
        if (instruction == 0xf9401008U) CHECK(env.address == 0x21);
    }
}

TEST_CASE("A64 mapped fastmem remains coherent and cross-page invalid stores do not partially commit", "[unmapped]") {
    Common::HostMemory memory(2 * 1024 * 1024, 64 * 1024 * 1024);
    REQUIRE(memory.VirtualBasePointer() != nullptr);
    memory.Map(0x1000, 0, 0x1000, Common::MemoryPermission::ReadWrite, false);
    auto* backing = memory.BackingBasePointer();
    std::fill_n(backing, 0x1000, u8{0x7b});
    FaultEnv env;
    env.code_mem_start_address = 0x20000;
    env.code_mem = {0xf9000008, 0x14000000}; // STR X8,[X0]
    Dynarmic::A64::UserConfig conf;
    conf.callbacks = &env;
    conf.code_cache_size = 16 * 1024 * 1024;
    conf.precise_memory_callback_pc = true;
    conf.check_halt_on_memory_access = true;
    conf.fastmem_pointer = reinterpret_cast<uintptr_t>(memory.VirtualBasePointer());
    conf.fastmem_address_space_bits = 26;
    conf.silently_mirror_fastmem = false;
    conf.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
    conf.only_detect_misalignment_via_page_table_on_page_boundary = true;
    Dynarmic::A64::Jit jit{conf};
    env.jit = &jit;
    jit.SetPC(0x20000);
    jit.SetRegister(0, 0x1000);
    jit.SetRegister(8, 0x12345678);
    env.ticks_left = 2;
    jit.Run();
    REQUIRE(env.count == 0);
    CHECK(backing[0] == 0x78);
    CHECK(backing[3] == 0x12);
    env.fault = true;
    for (unsigned retry = 0; retry < 2; ++retry) {
        jit.SetPC(0x20000);
        jit.SetRegister(0, 0x1ffc);
        env.ticks_left = 2;
        CHECK(Dynarmic::Has(jit.Run(), Dynarmic::HaltReason::MemoryAbort));
        CHECK(env.pc == 0x20000);
        CHECK(jit.GetPC() == 0x20000);
        CHECK(backing[0xffc] == 0x7b);
        CHECK(backing[0xfff] == 0x7b);
    }
    CHECK(env.count == 2);
}

TEST_CASE("A64 exclusive abort releases the monitor and preserves the destination", "[unmapped]") {
    // LDXRB W8,[X0]; LDXRH W8,[X0]; LDXR W8,[X0]; LDXR X8,[X0]; LDXP X8,X9,[X0].
    for (u32 instruction : {0x085f7c08U, 0x485f7c08U, 0x885f7c08U, 0xc85f7c08U, 0xc87f2408U}) {
        Dynarmic::ExclusiveMonitor monitor{1};
        FaultEnv env;
        env.fault = true;
        env.code_mem_start_address = 0x30000;
        env.code_mem = {instruction, 0x14000000};
        Dynarmic::A64::UserConfig conf;
        conf.callbacks = &env;
        conf.global_monitor = &monitor;
        conf.code_cache_size = 16 * 1024 * 1024;
        conf.precise_memory_callback_pc = true;
        conf.check_halt_on_memory_access = true;
        Dynarmic::A64::Jit jit{conf};
        env.jit = &jit;
        jit.SetRegister(0, 0x20);
        jit.SetRegister(8, 0xfeed);
        jit.SetRegister(9, 0xbeef);
        for (unsigned retry = 0; retry < 2; ++retry) {
            jit.SetPC(0x30000);
            env.ticks_left = 2;
            CHECK(Dynarmic::Has(jit.Run(), Dynarmic::HaltReason::MemoryAbort));
            CHECK(env.pc == 0x30000);
            CHECK(jit.GetRegister(8) == 0xfeed);
            CHECK(jit.GetRegister(9) == 0xbeef);
            jit.ClearExclusiveState(); // Would deadlock if the abort retained the monitor lock.
        }
        CHECK(env.count == 2);
    }
}

TEST_CASE("A64 exclusive stores report every width and abort before status writeback", "[unmapped]") {
    constexpr std::array<u32, 5> loads{0x085f7c08, 0x485f7c08, 0x885f7c08, 0xc85f7c08, 0xc87f2408};
    // STXR{B,H} W10,W8,[X0]; STXR W10,{W8,X8},[X0]; STXP W10,X8,X9,[X0].
    constexpr std::array<u32, 5> stores{0x080a7c08, 0x480a7c08, 0x880a7c08, 0xc80a7c08, 0xc82a2408};
    constexpr std::array<size_t, 5> sizes{1, 2, 4, 8, 16};
    Common::HostMemory memory(2 * 1024 * 1024, 64 * 1024 * 1024);
    REQUIRE(memory.VirtualBasePointer() != nullptr);
    for (bool fault : {false, true}) {
        for (size_t i = 0; i < loads.size(); ++i) {
            INFO("fault=" << fault << " width=" << sizes[i]);
            Dynarmic::ExclusiveMonitor monitor{1};
            FaultEnv env;
            env.fault = fault;
            env.abort_reads = false; // Reach the store with a successfully established reservation.
            env.code_mem_start_address = 0x40000;
            env.code_mem = {loads[i], stores[i], 0x9100056b, 0x14000000}; // ADD X11,X11,#1
            Dynarmic::A64::UserConfig conf;
            conf.callbacks = &env;
            conf.global_monitor = &monitor;
            conf.code_cache_size = 16 * 1024 * 1024;
            conf.precise_memory_callback_pc = true;
            conf.check_halt_on_memory_access = fault;
            if (!fault) {
                conf.fastmem_pointer = reinterpret_cast<uintptr_t>(memory.VirtualBasePointer());
                conf.fastmem_address_space_bits = 26;
                conf.silently_mirror_fastmem = false;
                conf.fastmem_exclusive_access = true;
            }
            Dynarmic::A64::Jit jit{conf};
            env.jit = &jit;
            for (unsigned retry = 0; retry < 2; ++retry) {
                jit.SetPC(0x40000);
                jit.SetRegister(0, 0x20);
                jit.SetRegister(10, 0xfeed);
                jit.SetRegister(11, 7);
                env.ticks_left = 4;
                const auto reason = jit.Run();
                CHECK(env.count == (retry + 1) * 2);
                CHECK(env.pc == 0x40004);
                CHECK(env.width == sizes[i]);
                CHECK(env.write);
                if (fault) {
                    CHECK(Dynarmic::Has(reason, Dynarmic::HaltReason::MemoryAbort));
                    CHECK(jit.GetPC() == 0x40004);
                    CHECK(jit.GetRegister(10) == 0xfeed);
                    CHECK(jit.GetRegister(11) == 7);
                } else {
                    CHECK(jit.GetRegister(11) == 8);
                }
                jit.ClearExclusiveState();
            }
        }
    }
}
