// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/common_types.h"

namespace Core::TraceExpressions {

using Registers = std::array<u64, 34>; // x0..x30, sp, pc, pstate
// Return true only when the whole span was read. Reads must not mutate guest state.
using MemoryReader = std::function<bool(u64, std::span<u8>)>;

inline constexpr std::size_t MaxExpressionLength = 256;
inline constexpr std::size_t MaxOperations = 32;
inline constexpr std::size_t MaxDereferences = 4;
inline constexpr std::size_t MaxCaptureBytes = 4096;

// atom := register | unsigned_integer | '[' expression ']'
// expression := atom (('+' | '-') unsigned_integer)*
// A '+' also accepts a signed constant, e.g. x0+-4. Offsets fit s64.
// Arithmetic is checked, never wrapping; each dereference reads a LE u64.
class Expression {
public:
    static std::optional<Expression> Parse(std::string_view text, std::string& error);
    // On failure, out is unchanged. Evaluation allocates no memory.
    bool Evaluate(const Registers& registers, const MemoryReader& read, u64& out) const;

private:
    friend class ExpressionParser;
    enum class Opcode { Constant, Register, Add, Subtract, Dereference };
    struct Operation {
        Opcode opcode;
        u64 operand;
    };
    Expression() = default;
    std::vector<Operation> operations;
};

enum class CaptureType { Raw, U8, U16, U32, U64, I32, F32, Bool, CString };
using CaptureValue = std::variant<std::vector<u8>, u64, s32, float, bool, std::string>;
struct CaptureResult {
    u64 address{};
    CaptureType type{CaptureType::Raw};
    CaptureValue value;
    // CString only: false means the N-byte limit was reached without a NUL.
    bool terminated{};
};

class Capture {
public:
    // Raw capture: address_expression[:N], default N=8; 1 <= N <= 4096.
    static std::optional<Capture> Parse(std::string_view text, std::string& error);
    // type: raw, u8/u16/u32/u64/i32/f32/bool/c-string(N).
    // Typed sizes come from type; :N is accepted only for raw captures.
    static std::optional<Capture> Parse(std::string_view text, std::string_view type,
                                       std::string& error);
    // The expression evaluates an address; then the requested bytes are read.
    // All values are little-endian; bool is one byte, nonzero => true.
    // CString stops at NUL or N bytes. On failure, out is unchanged.
    bool Evaluate(const Registers& registers, const MemoryReader& read, CaptureResult& out) const;
    const Expression& Address() const { return address; }
    CaptureType Type() const { return type; }
    std::size_t Size() const { return size; }

private:
    Capture(Expression address_, CaptureType type_, std::size_t size_);
    Expression address;
    CaptureType type;
    std::size_t size;
};

class Condition {
public:
    // One unsigned comparison: expression OP expression, or thread OP main/integer.
    // OP is == != < <= > >=. Empty conditions and compound predicates are invalid.
    static std::optional<Condition> Parse(std::string_view text, std::string& error);
    // Failed reads/arithmetic produce false, including for !=.
    bool Evaluate(const Registers& registers, const MemoryReader& read, u64 thread_id,
                  u64 main_thread_id) const;

private:
    enum class Comparison { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
    Condition() = default;
    Comparison comparison{Comparison::Equal};
    bool thread_condition{};
    bool main_thread{};
    u64 compared_thread{};
    std::optional<Expression> left;
    std::optional<Expression> right;
};

} // namespace Core::TraceExpressions
