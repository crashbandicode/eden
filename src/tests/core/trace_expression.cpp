// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include "core/debugger/trace_expression.h"

namespace {
using namespace Core::TraceExpressions;

void Require(bool condition, const std::source_location where = std::source_location::current()) {
    if (!condition) {
        std::cerr << "FAIL " << where.file_name() << ':' << where.line() << '\n';
        std::exit(EXIT_FAILURE);
    }
}

Expression ParseExpression(std::string_view text) {
    std::string error = "old error";
    auto expression = Expression::Parse(text, error);
    Require(expression.has_value() && error.empty());
    return *expression;
}

Capture ParseCapture(std::string_view text, std::string_view type = "raw") {
    std::string error = "old error";
    auto capture = Capture::Parse(text, type, error);
    Require(capture.has_value() && error.empty());
    return *capture;
}

Condition ParseCondition(std::string_view text) {
    std::string error = "old error";
    auto condition = Condition::Parse(text, error);
    Require(condition.has_value() && error.empty());
    return *condition;
}

struct Memory {
    static constexpr u64 Base = 0x1000;
    std::array<u8, MaxCaptureBytes> bytes{};
    unsigned reads{};

    MemoryReader Reader() {
        return [this](u64 address, std::span<u8> output) {
            ++reads;
            if (address < Base || address - Base > bytes.size() ||
                output.size() > bytes.size() - (address - Base)) {
                return false;
            }
            std::copy_n(bytes.begin() + (address - Base), output.size(), output.begin());
            return true;
        };
    }

    void Store64(std::size_t offset, u64 value) {
        for (unsigned i = 0; i < 8; ++i) {
            bytes[offset + i] = static_cast<u8>(value >> (i * 8));
        }
    }
};

void TestRegistersAndConstants() {
    Registers registers{};
    for (std::size_t i = 0; i < registers.size(); ++i) {
        registers[i] = 0x100 + i;
    }
    for (unsigned i = 0; i <= 30; ++i) {
        u64 value{};
        Require(ParseExpression("x" + std::to_string(i)).Evaluate(registers, {}, value));
        Require(value == registers[i]);
    }
    const std::array<std::string_view, 3> names{"sp", "pc", "pstate"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        u64 value{};
        Require(ParseExpression(names[i]).Evaluate(registers, {}, value));
        Require(value == registers[31 + i]);
    }
    for (const auto text : {"18446744073709551615", "0xffffffffffffffff", "0XFFFFFFFFFFFFFFFF"}) {
        u64 value{};
        Require(ParseExpression(text).Evaluate(registers, {}, value));
        Require(value == std::numeric_limits<u64>::max());
    }
    u64 value{9};
    Require(ParseExpression(" \t0 \r\n").Evaluate(registers, {}, value) && value == 0);
    Require(ParseExpression("010").Evaluate(registers, {}, value) && value == 10);
    std::cout << "PASS all registers and full-width decimal/hex constants\n";
}

void TestGrammarBounds() {
    const std::array invalid{
        "", " ", "x31", "x00", "x0x1", "w0", "SP", "lr", "-1", "+1", "0x", "0xg",
        "18446744073709551616", "0x10000000000000000", "x0+x1", "x0+", "x0--1",
        "[x0", "x0]", "[]", "[[]]", "(x0)", "x0*2", "x0/2", "x0:8", "x0;pc",
        "[[[[[x0]]]]]", "x0 + 9223372036854775808", "x0 - 9223372036854775809",
        "x0 + -9223372036854775809", "x0 1", "[x0][x1]", "x0 + 0x1z"};
    for (const auto text : invalid) {
        std::string error;
        Require(!Expression::Parse(text, error) && !error.empty());
    }
    std::string error;
    Require(!Expression::Parse(std::string_view{"x0\0+1", 5}, error));
    std::string longest = "x0" + std::string(MaxExpressionLength - 2, ' ');
    Require(Expression::Parse(longest, error).has_value());
    Require(!Expression::Parse(longest + " ", error));
    std::string operations = "x0";
    for (std::size_t i = 1; i < MaxOperations; ++i) {
        operations += "+0";
    }
    Require(Expression::Parse(operations, error).has_value());
    Require(!Expression::Parse(operations + "+0", error));
    Require(Expression::Parse("[[[[x0]]]]", error).has_value());
    std::cout << "PASS malformed grammar, integer overflow, length/operation/depth bounds\n";
}

void TestArithmetic() {
    Registers registers{};
    registers[0] = 10;
    u64 value{};
    Require(ParseExpression(" x0 + 0x10 - 2 + -4 ").Evaluate(registers, {}, value));
    Require(value == 20);
    Require(ParseExpression("x0 + +4").Evaluate(registers, {}, value) && value == 14);
    Require(ParseExpression("0x8000000000000000-9223372036854775808")
                .Evaluate(registers, {}, value) && value == 0);
    Require(ParseExpression("0+9223372036854775807").Evaluate(registers, {}, value));
    Require(value == 0x7fffffffffffffffULL);
    for (const auto text : {"0-1", "0xffffffffffffffff+1", "x0-11",
                            "0xffffffffffffffff+1-1"}) {
        value = 0xfeed;
        Require(!ParseExpression(text).Evaluate(registers, {}, value));
        Require(value == 0xfeed);
    }
    std::cout << "PASS signed offsets, exact limits, overflow/underflow without wrapping\n";
}

void TestPointers() {
    Registers registers{};
    registers[0] = Memory::Base;
    Memory memory;
    memory.Store64(0x10, Memory::Base + 0x20);
    memory.Store64(0x28, 0x1122334455667788ULL);
    u64 value{};
    Require(ParseExpression("[[x0+0x10]+0x8]").Evaluate(registers, memory.Reader(), value));
    Require(value == 0x1122334455667788ULL && memory.reads == 2);
    memory.Store64(0, Memory::Base);
    memory.reads = 0;
    Require(ParseExpression("[[[[x0]]]]").Evaluate(registers, memory.Reader(), value));
    Require(value == Memory::Base && memory.reads == MaxDereferences);

    unsigned calls{};
    const MemoryReader partial = [&](u64, std::span<u8> bytes) {
        ++calls;
        bytes.front() = 0x55;
        return false;
    };
    value = 123;
    Require(!ParseExpression("[[x0]]").Evaluate(registers, partial, value));
    Require(calls == 1 && value == 123);
    Require(!ParseExpression("[x0]").Evaluate(registers, {}, value) && value == 123);
    calls = 0;
    Require(!ParseExpression("[0xfffffffffffffff9]").Evaluate(registers, partial, value));
    Require(calls == 0 && value == 123);
    const MemoryReader last_word = [&](u64 address, std::span<u8> bytes) {
        Require(address == 0xfffffffffffffff8ULL && bytes.size() == 8);
        std::fill(bytes.begin(), bytes.end(), 0xff);
        return true;
    };
    Require(ParseExpression("[0xfffffffffffffff8]").Evaluate(registers, last_word, value));
    Require(value == std::numeric_limits<u64>::max());
    std::cout << "PASS little-endian pointer chasing, four reads, failed/range-overflow reads\n";
}

void TestRawCaptures() {
    Registers registers{};
    registers[0] = Memory::Base;
    Memory memory;
    memory.Store64(0x10, Memory::Base + 0x20);
    memory.Store64(0x28, Memory::Base + 0x80);
    for (unsigned i = 0; i < 16; ++i) {
        memory.bytes[0x80 + i] = static_cast<u8>(i + 1);
    }
    CaptureResult result;
    auto capture = ParseCapture("[[x0+0x10]+0x8]:16");
    Require(capture.Size() == 16 && capture.Type() == CaptureType::Raw);
    Require(capture.Evaluate(registers, memory.Reader(), result));
    Require(result.address == Memory::Base + 0x80 && result.type == CaptureType::Raw);
    Require(std::get<std::vector<u8>>(result.value) ==
            std::vector<u8>({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}));
    Require(memory.reads == 3);
    std::string error;
    auto default_capture = Capture::Parse("x0", error);
    Require(default_capture && error.empty() && default_capture->Size() == 8);
    Require(default_capture->Evaluate(registers, memory.Reader(), result));
    Require(std::get<std::vector<u8>>(result.value).size() == 8);
    Require(ParseCapture("x0:4096").Evaluate(registers, memory.Reader(), result));
    Require(std::get<std::vector<u8>>(result.value).size() == MaxCaptureBytes);
    Require(ParseCapture("x0:0x1").Evaluate(registers, memory.Reader(), result));
    Require(std::get<std::vector<u8>>(result.value).size() == 1);
    std::cout << "PASS raw captures, requested double-indirect example, default/min/max sizes\n";
}

void TestTypedCaptures() {
    Registers registers{};
    registers[0] = Memory::Base;
    Memory memory;
    memory.Store64(0, 0xfedcba9876543210ULL);
    const std::array<std::string_view, 4> types{"u8", "u16", "u32", "u64"};
    const std::array<u64, 4> expected{0x10, 0x3210, 0x76543210, 0xfedcba9876543210ULL};
    CaptureResult result;
    for (std::size_t i = 0; i < types.size(); ++i) {
        Require(ParseCapture("x0", types[i]).Evaluate(registers, memory.Reader(), result));
        Require(std::get<u64>(result.value) == expected[i]);
    }
    memory.Store64(0, 0xfffffffe);
    Require(ParseCapture("x0", "i32").Evaluate(registers, memory.Reader(), result));
    Require(std::get<s32>(result.value) == -2 && result.type == CaptureType::I32);
    memory.Store64(0, 0x80000000);
    Require(ParseCapture("x0", "i32").Evaluate(registers, memory.Reader(), result));
    Require(std::get<s32>(result.value) == std::numeric_limits<s32>::min());
    memory.Store64(0, 0x3fc00000);
    Require(ParseCapture("x0", "f32").Evaluate(registers, memory.Reader(), result));
    Require(std::get<float>(result.value) == 1.5f);
    memory.Store64(0, 0x80000000);
    Require(ParseCapture("x0", "f32").Evaluate(registers, memory.Reader(), result));
    Require(std::get<float>(result.value) == 0.0f && std::signbit(std::get<float>(result.value)));
    for (const u8 byte : {u8{0}, u8{1}, u8{0xff}}) {
        memory.bytes[0] = byte;
        Require(ParseCapture("x0", "bool").Evaluate(registers, memory.Reader(), result));
        Require(std::get<bool>(result.value) == (byte != 0));
    }
    std::cout << "PASS unsigned widths, signed i32, f32 bit decode, and one-byte bool\n";
}

void TestStrings() {
    Registers registers{};
    registers[0] = Memory::Base;
    Memory memory;
    memory.bytes[0] = 'h';
    memory.bytes[1] = 'i';
    CaptureResult result;
    Require(ParseCapture("x0", "c-string(16)").Evaluate(registers, memory.Reader(), result));
    Require(std::get<std::string>(result.value) == "hi" && result.terminated);
    Require(memory.reads == 3);
    Require(ParseCapture("x0", "c-string(2)").Evaluate(registers, memory.Reader(), result));
    Require(std::get<std::string>(result.value) == "hi" && !result.terminated);
    registers[0] = Memory::Base + memory.bytes.size() - 1;
    Require(ParseCapture("x0", "c-string(4096)").Evaluate(registers, memory.Reader(), result));
    Require(std::get<std::string>(result.value).empty() && result.terminated);
    memory.bytes.back() = 'z';
    result.address = 42;
    result.value = std::string{"preserved"};
    Require(!ParseCapture("x0", "c-string(2)").Evaluate(registers, memory.Reader(), result));
    Require(result.address == 42 && std::get<std::string>(result.value) == "preserved");
    registers[0] = Memory::Base;
    memory.bytes.fill('a');
    memory.reads = 0;
    Require(ParseCapture("x0", "c-string(4096)").Evaluate(registers, memory.Reader(), result));
    Require(std::get<std::string>(result.value) == std::string(MaxCaptureBytes, 'a'));
    Require(!result.terminated && memory.reads == MaxCaptureBytes);
    std::cout << "PASS NUL termination, bounded truncation, page-edge stop and partial failure\n";
}

void TestCaptureRejectionAndFailure() {
    std::string error;
    for (const auto text : {"x0:0", "x0:4097", "x0:-1", "x0:", "x0:8:8",
                            "x0:18446744073709551616"}) {
        Require(!Capture::Parse(text, error) && !error.empty());
    }
    for (const auto type : {"", "u128", "cstring(8)", "c-string(0)", "c-string(4097)",
                            "c-string(-1)", "c-string(8)junk", "c-string()"}) {
        Require(!Capture::Parse("x0", type, error) && !error.empty());
    }
    Require(!Capture::Parse("x0:4", "u32", error));
    Require(!Capture::Parse(std::string(300, ' '), error));
    Require(!Capture::Parse("x0", std::string(33, ' '), error));
    Registers registers{};
    CaptureResult result;
    result.address = 123;
    result.type = CaptureType::CString;
    result.value = std::string{"old"};
    result.terminated = true;
    unsigned calls{};
    const MemoryReader partial = [&](u64, std::span<u8> bytes) {
        ++calls;
        bytes[0] = 0xff;
        return false;
    };
    for (const auto type : {"raw", "u64", "i32", "f32", "bool", "c-string(2)"}) {
        Require(!ParseCapture("x0", type).Evaluate(registers, partial, result));
        Require(result.address == 123 && result.type == CaptureType::CString && result.terminated);
        Require(std::get<std::string>(result.value) == "old");
    }
    calls = 0;
    Require(!ParseCapture("0xffffffffffffffff:2").Evaluate(registers, partial, result));
    Require(!ParseCapture("0xffffffffffffffff+1").Evaluate(registers, partial, result));
    Require(calls == 0);
    const MemoryReader last_byte = [](u64 address, std::span<u8> bytes) {
        Require(address == std::numeric_limits<u64>::max() && bytes.size() == 1);
        bytes[0] = 0;
        return true;
    };
    Require(ParseCapture("0xffffffffffffffff", "u8").Evaluate(registers, last_byte, result));
    Require(std::get<u64>(result.value) == 0);
    Require(ParseCapture("0xffffffffffffffff", "c-string(2)")
                .Evaluate(registers, last_byte, result));
    Require(result.terminated && std::get<std::string>(result.value).empty());
    calls = 0;
    const MemoryReader nonzero_last_byte = [&](u64 address, std::span<u8> bytes) {
        ++calls;
        Require(address == std::numeric_limits<u64>::max() && bytes.size() == 1);
        bytes[0] = 'z';
        return true;
    };
    Require(!ParseCapture("0xffffffffffffffff", "c-string(2)")
                 .Evaluate(registers, nonzero_last_byte, result));
    Require(calls == 1 && result.terminated && std::get<std::string>(result.value).empty());
    std::cout << "PASS capture input bounds, complete failure isolation and final-address byte\n";
}

void TestConditions() {
    Registers registers{};
    registers[0] = Memory::Base;
    registers[1] = 3;
    registers[2] = std::numeric_limits<u64>::max();
    Memory memory;
    memory.Store64(0x30, 3);
    const auto read = memory.Reader();
    for (const auto text : {"x1 == 3", "[x0+0x30] != 0", "x1 < 4", "x1 <= 3",
                            "x1 > 2", "x1 >= 3", "[x0+0x30] == x1", "x2 > 0",
                            "x1+1 == 4", "3==3"}) {
        Require(ParseCondition(text).Evaluate(registers, read, 9, 7));
    }
    for (const auto text : {"x1 != 3", "x1 == 4", "x1 < 3", "x1 <= 2", "x1 > 3",
                            "x1 >= 4", "x2 < 0"}) {
        Require(!ParseCondition(text).Evaluate(registers, read, 9, 7));
    }
    const auto main = ParseCondition("thread == main");
    Require(main.Evaluate(registers, {}, 7, 7));
    Require(!main.Evaluate(registers, {}, 8, 7));
    Require(ParseCondition("thread != main").Evaluate(registers, {}, 8, 7));
    Require(ParseCondition("thread == 0x9").Evaluate(registers, {}, 9, 7));
    Require(ParseCondition("thread >= 9").Evaluate(registers, {}, 9, 7));
    Require(ParseCondition("thread == 18446744073709551615")
                .Evaluate(registers, {}, std::numeric_limits<u64>::max(), 7));
    std::cout << "PASS all unsigned comparisons, memory/register operands and thread selectors\n";
}

void TestConditionRejectionAndFailure() {
    std::string error;
    for (const auto text : {"", "x0", "x0 = 1", "x0 ! 1", "x0 === 1", "x0 <> 1",
                            "x0 ==", "== 1", "thread == x0", "thread == -1",
                            "main == thread", "x0 == main", "thread == main+1",
                            "thread == 18446744073709551616", "x0 == 1 && x1 == 2",
                            "x0 == 1 || x1 == 2", "x0 == 1 == 1"}) {
        Require(!Condition::Parse(text, error) && !error.empty());
    }
    Require(!Condition::Parse(std::string(517, ' '), error));
    Registers registers{};
    registers[0] = 0x1000;
    registers[1] = std::numeric_limits<u64>::max();
    unsigned calls{};
    const MemoryReader partial = [&](u64, std::span<u8> bytes) {
        ++calls;
        bytes[0] = 1;
        return false;
    };
    Require(!ParseCondition("[x0] != 0").Evaluate(registers, partial, 1, 1));
    Require(calls == 1);
    Require(!ParseCondition("1 != [x0]").Evaluate(registers, partial, 1, 1));
    Require(calls == 2);
    Require(!ParseCondition("[x0] == [x0]").Evaluate(registers, partial, 1, 1));
    Require(calls == 3); // Stop after failed left operand.
    Require(!ParseCondition("x1+1 == 0").Evaluate(registers, partial, 1, 1));
    Require(!ParseCondition("0-1 != 0").Evaluate(registers, partial, 1, 1));
    Require(calls == 3);
    std::cout << "PASS invalid/compound conditions and failed operands always false\n";
}

} // namespace

int main() {
    TestRegistersAndConstants();
    TestGrammarBounds();
    TestArithmetic();
    TestPointers();
    TestRawCaptures();
    TestTypedCaptures();
    TestStrings();
    TestCaptureRejectionAndFailure();
    TestConditions();
    TestConditionRejectionAndFailure();
    std::cout << "PASS all 10 trace expression test groups\n";
}
