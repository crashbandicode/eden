// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "dynarmic/tests/A64/testenv.h"
#include "dynarmic/tests/native/testenv.h"
#include "dynarmic/interface/A64/a64.h"

using namespace Dynarmic;

namespace {

/// Guest code like a game's dynamically loaded modules: a caller that stays loaded and a module of
/// many linked blocks that is unloaded (invalidated) and loaded again, at the same or a new address.
class ModuleEnv final : public A64TestEnv {
public:
    static constexpr u64 fixed_caller = 0x1000;     // MOV X0, 0; BL fixed_module; B .
    static constexpr u64 indirect_caller = 0x2000;  // MOV X0, 0; BLR X1; B .
    static constexpr u64 fixed_module = 0x100000;
    static constexpr size_t blocks = 512;
    static constexpr size_t block_words = 5;  // 4 x ADD X0, X0, imm; then B next (RET in the last)

    u64 module_base = 0;
    std::vector<u32> module;

    void Load(u64 base, size_t generation) {
        module_base = base;
        module.clear();
        for (size_t block = 0; block < blocks; block++) {
            for (size_t add = 0; add < 4; add++) {
                module.push_back(0x91000000 | u32(Immediate(generation, block, add) << 10));
            }
            module.push_back(block + 1 == blocks ? 0xd65f03c0 : 0x14000001);  // RET : B +4
        }
    }

    static u64 Immediate(size_t generation, size_t block, size_t add) {
        return (generation * 7 + block * 3 + add) % 4096;
    }

    static u64 Expected(size_t generation) {
        u64 sum = 0;
        for (size_t block = 0; block < blocks; block++) {
            for (size_t add = 0; add < 4; add++) {
                sum += Immediate(generation, block, add);
            }
        }
        return sum;
    }

    u64 ModuleSize() const { return module.size() * 4; }

    std::optional<std::uint32_t> MemoryReadCode(u64 vaddr) override {
        if (vaddr >= module_base && vaddr < module_base + ModuleSize()) {
            return module[(vaddr - module_base) / 4];
        }
        const std::array<u32, 3> fixed{0xd2800000, 0x94000000 | u32(((fixed_module - fixed_caller - 4) / 4) & 0x3ffffff), 0x14000000};
        const std::array<u32, 3> indirect{0xd2800000, 0xd63f0020, 0x14000000};
        if (vaddr >= fixed_caller && vaddr < fixed_caller + 12) {
            return fixed[(vaddr - fixed_caller) / 4];
        }
        if (vaddr >= indirect_caller && vaddr < indirect_caller + 12) {
            return indirect[(vaddr - indirect_caller) / 4];
        }
        return 0x14000000;  // B .
    }
};

/// Loads, runs and unloads the module `iterations` times; even generations reuse one address.
template<typename Observe>
void CycleModules(A64::Jit& jit, ModuleEnv& env, size_t iterations, bool reuse_address, Observe&& observe) {
    for (size_t generation = 0; generation < iterations; generation++) {
        const bool fixed = reuse_address && generation % 2 == 0;
        env.Load(fixed ? ModuleEnv::fixed_module : 0x10000000 + generation * 0x4000, generation);
        jit.SetRegister(1, env.module_base);
        jit.SetPC(fixed ? ModuleEnv::fixed_caller : ModuleEnv::indirect_caller);
        env.ticks_left = ModuleEnv::blocks * ModuleEnv::block_words + 16;
        CheckedRun([&] { jit.Run(); });
        REQUIRE(jit.GetRegister(0) == ModuleEnv::Expected(generation));
        jit.InvalidateCacheRange(env.module_base, env.ModuleSize());
        observe(generation);
    }
}

}  // namespace

TEST_CASE("A64 code that is unloaded and loaded again keeps running correctly", "[a64]") {
    // A small code cache also fills up, so released code pages are written again after ClearCache.
    ModuleEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    conf.code_cache_size = 8 * 1024 * 1024;
    A64::Jit jit{conf};
    CycleModules(jit, env, 300, true, [](size_t) {});
}

TEST_CASE("A64 unloaded code does not keep its bookkeeping", "[a64]") {
#if defined(__GLIBC__)
    ModuleEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};
    size_t baseline = 0;
    CycleModules(jit, env, 600, false, [&](size_t generation) {
        if (generation == 100) {
            baseline = mallinfo2().uordblks;
        }
    });
    // While invalidated blocks kept their patch places, ranges and patch records, this grew with
    // every unloaded block: 500 modules of 512 blocks here.
    const auto grown = std::ptrdiff_t(mallinfo2().uordblks) - std::ptrdiff_t(baseline);
    INFO("heap grew by " << grown << " bytes");
    REQUIRE(grown < 4 * 1024 * 1024);
#else
    SKIP("needs glibc's mallinfo2");
#endif
}

TEST_CASE("A64 tracepoint interrupts cached code before the instruction and resumes", "[a64][tracepoint]") {
    class TraceEnv final : public A64TestEnv {
    public:
        A64::Jit* jit{};
        bool armed{};
        u64 address{};
        u64 hit_pc{};
        u64 hit_x0{};
        unsigned hits{};
        bool IsTracepoint(u64 pc) override {
            return armed && pc == address;
        }
        void ExceptionRaised(u64 pc, A64::Exception exception) override {
            REQUIRE(exception == A64::Exception::Tracepoint);
            hit_pc = pc;
            hit_x0 = jit->GetRegister(0);
            ++hits;
            jit->HaltExecution(HaltReason::UserDefined5);
        }
    } env;
    env.address = GENERATE(100ULL, 108ULL);
    env.code_mem_start_address = 100;
    env.code_mem = {0xd28000e0,  // MOV X0, 7
                    0x1e211802,  // FDIV S2, S0, S1 (1 / 0 raises FPSR.DZC)
                    0x91000400,  // ADD X0, X0, 1
                    0xf9000020,  // STR X0, [X1]
                    0x14000000}; // B .
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};
    env.jit = &jit;
    jit.SetRegister(1, 0x2000);
    jit.SetVector(0, {0x3f800000, 0}); // 1.0f
    jit.SetVector(1, {0, 0});
    jit.SetPC(100);
    env.ticks_left = 5;
    CheckedRun([&] { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 8);
    REQUIRE(env.MemoryRead(0x2000, 8) == 8);
    REQUIRE(env.hits == 0);

    // Arming invalidates a block that has already executed, including its
    // linked/fast-dispatch entry. Cover both a zero-instruction block and a
    // prefix that raises a host floating-point exception before the hit.
    env.armed = true;
    jit.InvalidateCacheRange(env.address, 4);
    jit.SetFpsr(0);
    jit.SetRegister(0, 99);
    jit.SetPC(100);
    env.ticks_left = 5;
    HaltReason reason{};
    CheckedRun([&] { reason = jit.Run(); });
    REQUIRE(Has(reason, HaltReason::UserDefined5));
    REQUIRE(env.hits == 1);
    REQUIRE(env.hit_pc == env.address);
    REQUIRE(env.hit_x0 == (env.address == 100 ? 99 : 7));
    REQUIRE(jit.GetRegister(0) == env.hit_x0);
    REQUIRE(jit.GetPC() == env.address + 4);
    const auto synced_fpsr = jit.GetFpsr();
    REQUIRE((synced_fpsr & 2) == (env.address == 100 ? 0 : 2));

    // Same resume sequence used by PhysicalCore: rewind to the saved PC,
    // disarm and invalidate, then run without a client resume command.
    jit.SetPC(jit.GetPC() - 4);
    jit.SetFpsr(synced_fpsr);
    env.armed = false;
    jit.InvalidateCacheRange(env.hit_pc, 4);
    env.ticks_left = 5;
    CheckedRun([&] { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 8);
    REQUIRE(env.MemoryRead(0x2000, 8) == 8);
    REQUIRE(jit.GetPC() == 116);
    REQUIRE((jit.GetFpsr() & 2) == 2);
    REQUIRE(env.hits == 1);
    REQUIRE(env.code_mem == std::vector<u32>{0xd28000e0, 0x1e211802, 0x91000400, 0xf9000020, 0x14000000});
}

TEST_CASE("A64 persistent tracepoint executes every loop instruction exactly once", "[a64][tracepoint]") {
    class TraceEnv final : public A64TestEnv {
    public:
        A64::Jit* jit{};
        unsigned hits{};
        bool IsTracepoint(u64 pc) override { return pc == 104 || pc == 108; }
        void ExceptionRaised(u64, A64::Exception exception) override {
            REQUIRE(exception == A64::Exception::Tracepoint);
            ++hits;
            jit->HaltExecution(HaltReason::UserDefined5);
        }
    } env;
    // MOV X0,0; ADD X0,X0,1; STR X0,[X1]; B back to ADD.
    env.code_mem_start_address = 100;
    env.code_mem = {0xd2800000, 0x91000400, 0xf9000020, 0x17fffffe};
    const auto original = env.code_mem;
    A64::UserConfig config{};
    config.callbacks = &env;
    A64::Jit jit{config};
    env.jit = &jit;
    jit.SetPC(100);
    jit.SetRegister(1, 0x2000);
    for (unsigned hit = 0; hit < 12; ++hit) {
        env.ticks_left = 100;
        HaltReason reason{};
        CheckedRun([&] { reason = jit.Run(); });
        REQUIRE(Has(reason, HaltReason::UserDefined5));
        REQUIRE(env.hits == hit + 1);
        const auto pc = jit.GetPC() - 4;
        REQUIRE(pc == (hit % 2 ? 108 : 104));
        jit.SetPC(pc);
        CheckedRun([&] { reason = jit.Step(); });
        REQUIRE(Has(reason, HaltReason::Step));
        REQUIRE(env.hits == hit + 1);
        REQUIRE(jit.GetRegister(0) == hit / 2 + 1);
        if (hit % 2) REQUIRE(env.MemoryRead(0x2000, 8) == hit / 2 + 1);
    }
    REQUIRE(env.code_mem == original);
}

TEST_CASE("ensure fast dispatch entry is cleared even when a block does not have any patching requirements", "[a64]") {
    A64TestEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};

    REQUIRE(conf.HasOptimization(OptimizationFlag::FastDispatch));

    env.code_mem_start_address = 100;
    env.code_mem.clear();
    env.code_mem.emplace_back(0xd2800d80);  // MOV X0, 108
    env.code_mem.emplace_back(0xd61f0000);  // BR X0
    env.code_mem.emplace_back(0xd2800540);  // MOV X0, 42
    env.code_mem.emplace_back(0x14000000);  // B .

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.InvalidateCacheRange(108, 4);

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    env.code_mem[2] = 0xd28008a0;  // MOV X0, 69

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.InvalidateCacheRange(108, 4);

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 69);

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 69);
}

TEST_CASE("ensure fast dispatch entry is cleared even when a block does not have any patching requirements 2", "[a64]") {
    A64TestEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};

    REQUIRE(conf.HasOptimization(OptimizationFlag::FastDispatch));

    env.code_mem.emplace_back(0xd2800100);  // MOV X0, 8
    env.code_mem.emplace_back(0xd61f0000);  // BR X0
    env.code_mem.emplace_back(0xd2800540);  // MOV X0, 42
    env.code_mem.emplace_back(0x14000000);  // B .

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.InvalidateCacheRange(8, 4);

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    env.code_mem[2] = 0xd28008a0;  // MOV X0, 69

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.InvalidateCacheRange(8, 4);

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 69);

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 69);
}
