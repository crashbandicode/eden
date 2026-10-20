// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "dynarmic/tests/A64/testenv.h"
#include "dynarmic/tests/native/testenv.h"
#include "dynarmic/interface/A64/a64.h"

using namespace Dynarmic;

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
