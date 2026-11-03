// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/debugger/trace_expression.h"

#include <bit>
#include <charconv>
#include <limits>
#include <utility>

namespace Core::TraceExpressions {
namespace {

constexpr u64 MaxValue = std::numeric_limits<u64>::max();

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && IsSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool ParseInteger(std::string_view text, u64& value) {
    text = Trim(text);
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty() || text.front() == '+' || text.front() == '-') {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool ReadBytes(const MemoryReader& read, u64 address, std::span<u8> bytes) {
    return read && !bytes.empty() && bytes.size() - 1 <= MaxValue - address &&
           read(address, bytes);
}

u64 DecodeUnsigned(std::span<const u8> bytes) {
    u64 value{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value |= static_cast<u64>(bytes[i]) << (i * 8);
    }
    return value;
}

} // namespace

class ExpressionParser {
public:
    ExpressionParser(std::string_view text_, std::string& error_) : text{text_}, error{error_} {}

    std::optional<Expression> Parse() {
        error.clear();
        if (text.size() > MaxExpressionLength) {
            Fail("expression exceeds 256 characters");
            return std::nullopt;
        }
        if (!ParseExpression(0)) {
            return std::nullopt;
        }
        SkipSpace();
        if (position != text.size()) {
            Fail("unexpected expression token");
            return std::nullopt;
        }
        return std::move(expression);
    }

private:
    bool Fail(std::string_view message) {
        error = std::string{message} + " at byte " + std::to_string(position);
        return false;
    }

    void SkipSpace() {
        while (position < text.size() && IsSpace(text[position])) {
            ++position;
        }
    }

    bool Emit(Expression::Opcode opcode, u64 operand = 0) {
        if (expression.operations.size() == MaxOperations) {
            return Fail("expression exceeds 32 operations");
        }
        expression.operations.push_back({opcode, operand});
        return true;
    }

    std::string_view Token() {
        SkipSpace();
        const auto start = position;
        while (position < text.size()) {
            const char c = text[position];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9'))) {
                break;
            }
            ++position;
        }
        return text.substr(start, position - start);
    }

    bool ParseExpression(std::size_t depth) {
        using Opcode = Expression::Opcode;
        SkipSpace();
        if (position < text.size() && text[position] == '[') {
            if (depth == MaxDereferences) {
                return Fail("expression exceeds four dereferences");
            }
            ++position;
            if (!ParseExpression(depth + 1)) {
                return false;
            }
            SkipSpace();
            if (position == text.size() || text[position] != ']') {
                return Fail("expected closing bracket");
            }
            ++position;
            if (!Emit(Opcode::Dereference)) {
                return false;
            }
        } else {
            const auto token = Token();
            u64 value{};
            if (token == "sp" || token == "pc" || token == "pstate") {
                value = token == "sp" ? 31 : token == "pc" ? 32 : 33;
                if (!Emit(Opcode::Register, value)) {
                    return false;
                }
            } else if (token.starts_with('x')) {
                const auto index = token.substr(1);
                if (index.empty() || (index.size() > 1 && index.front() == '0') ||
                    !ParseInteger(index, value) || value > 30 ||
                    index.find_first_not_of("0123456789") != std::string_view::npos) {
                    return Fail("expected register x0..x30, sp, pc or pstate");
                }
                if (!Emit(Opcode::Register, value)) {
                    return false;
                }
            } else if (!ParseInteger(token, value)) {
                return Fail("expected register, u64 integer or bracketed expression");
            } else if (!Emit(Opcode::Constant, value)) {
                return false;
            }
        }

        for (;;) {
            SkipSpace();
            if (position == text.size() || (text[position] != '+' && text[position] != '-')) {
                return true;
            }
            bool negative = text[position++] == '-';
            SkipSpace();
            if (!negative && position < text.size() &&
                (text[position] == '-' || text[position] == '+')) {
                negative = text[position++] == '-';
            }
            u64 magnitude{};
            if (!ParseInteger(Token(), magnitude) ||
                magnitude > static_cast<u64>(std::numeric_limits<s64>::max()) + (negative ? 1 : 0)) {
                return Fail("expected constant offset fitting s64");
            }
            if (!Emit(negative ? Opcode::Subtract : Opcode::Add, magnitude)) {
                return false;
            }
        }
    }

    std::string_view text;
    std::string& error;
    std::size_t position{};
    Expression expression;
};

std::optional<Expression> Expression::Parse(std::string_view text, std::string& error) {
    return ExpressionParser{text, error}.Parse();
}

bool Expression::Evaluate(const Registers& registers, const MemoryReader& read, u64& out) const {
    u64 value{};
    for (const auto& operation : operations) {
        switch (operation.opcode) {
        case Opcode::Constant:
            value = operation.operand;
            break;
        case Opcode::Register:
            value = registers[operation.operand];
            break;
        case Opcode::Add:
            if (operation.operand > MaxValue - value) {
                return false;
            }
            value += operation.operand;
            break;
        case Opcode::Subtract:
            if (operation.operand > value) {
                return false;
            }
            value -= operation.operand;
            break;
        case Opcode::Dereference: {
            std::array<u8, 8> bytes{};
            if (!ReadBytes(read, value, bytes)) {
                return false;
            }
            value = DecodeUnsigned(bytes);
            break;
        }
        }
    }
    out = value;
    return true;
}

Capture::Capture(Expression address_, CaptureType type_, std::size_t size_)
    : address{std::move(address_)}, type{type_}, size{size_} {}

std::optional<Capture> Capture::Parse(std::string_view text, std::string& error) {
    return Parse(text, "raw", error);
}

std::optional<Capture> Capture::Parse(std::string_view text, std::string_view type_text,
                                     std::string& error) {
    error.clear();
    // Bound all input before scanning or trimming it.
    if (text.size() > MaxExpressionLength + 16 || type_text.size() > 32) {
        error = "capture specification is too long";
        return std::nullopt;
    }
    text = Trim(text);
    type_text = Trim(type_text);
    CaptureType type = CaptureType::Raw;
    u64 size = 8;
    if (type_text == "u8") {
        type = CaptureType::U8;
        size = 1;
    } else if (type_text == "u16") {
        type = CaptureType::U16;
        size = 2;
    } else if (type_text == "u32") {
        type = CaptureType::U32;
        size = 4;
    } else if (type_text == "u64") {
        type = CaptureType::U64;
    } else if (type_text == "i32") {
        type = CaptureType::I32;
        size = 4;
    } else if (type_text == "f32") {
        type = CaptureType::F32;
        size = 4;
    } else if (type_text == "bool") {
        type = CaptureType::Bool;
        size = 1;
    } else if (type_text.starts_with("c-string(") && type_text.ends_with(')')) {
        type = CaptureType::CString;
        if (!ParseInteger(type_text.substr(9, type_text.size() - 10), size)) {
            error = "invalid c-string byte limit";
            return std::nullopt;
        }
    } else if (type_text != "raw") {
        error = "unknown capture type";
        return std::nullopt;
    }

    const auto colon = text.find(':');
    if (colon != std::string_view::npos) {
        if (type != CaptureType::Raw || !ParseInteger(text.substr(colon + 1), size)) {
            error = "only raw captures accept :N with an integer byte count";
            return std::nullopt;
        }
        text = text.substr(0, colon);
    }
    if (size == 0 || size > MaxCaptureBytes) {
        error = "capture byte count must be 1..4096";
        return std::nullopt;
    }
    auto expression = Expression::Parse(text, error);
    if (!expression) {
        return std::nullopt;
    }
    return Capture{std::move(*expression), type, static_cast<std::size_t>(size)};
}

bool Capture::Evaluate(const Registers& registers, const MemoryReader& read,
                       CaptureResult& out) const {
    CaptureResult result;
    result.type = type;
    if (!address.Evaluate(registers, read, result.address)) {
        return false;
    }
    if (type == CaptureType::CString) {
        std::string text;
        text.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            std::array<u8, 1> byte{};
            if (i > MaxValue - result.address || !ReadBytes(read, result.address + i, byte)) {
                return false;
            }
            if (byte[0] == 0) {
                result.terminated = true;
                break;
            }
            text.push_back(static_cast<char>(byte[0]));
        }
        result.value = std::move(text);
    } else if (type == CaptureType::Raw) {
        std::vector<u8> bytes(size);
        if (!ReadBytes(read, result.address, bytes)) {
            return false;
        }
        result.value = std::move(bytes);
    } else {
        std::array<u8, 8> bytes{};
        const auto view = std::span{bytes}.first(size);
        if (!ReadBytes(read, result.address, view)) {
            return false;
        }
        const u64 value = DecodeUnsigned(view);
        switch (type) {
        case CaptureType::I32:
            result.value = std::bit_cast<s32>(static_cast<u32>(value));
            break;
        case CaptureType::F32:
            result.value = std::bit_cast<float>(static_cast<u32>(value));
            break;
        case CaptureType::Bool:
            result.value = value != 0;
            break;
        default:
            result.value = value;
            break;
        }
    }
    out = std::move(result);
    return true;
}

std::optional<Condition> Condition::Parse(std::string_view text, std::string& error) {
    error.clear();
    if (text.size() > 2 * MaxExpressionLength + 4) {
        error = "condition is too long";
        return std::nullopt;
    }
    const auto position = text.find_first_of("=!<>");
    if (position == std::string_view::npos) {
        error = "expected comparison operator";
        return std::nullopt;
    }
    Condition condition;
    std::size_t length = 1;
    if (position + 1 < text.size() && text[position + 1] == '=') {
        length = 2;
    }
    const auto op = text.substr(position, length);
    if (op == "==") {
        condition.comparison = Comparison::Equal;
    } else if (op == "!=") {
        condition.comparison = Comparison::NotEqual;
    } else if (op == "<") {
        condition.comparison = Comparison::Less;
    } else if (op == "<=") {
        condition.comparison = Comparison::LessEqual;
    } else if (op == ">") {
        condition.comparison = Comparison::Greater;
    } else if (op == ">=") {
        condition.comparison = Comparison::GreaterEqual;
    } else {
        error = "invalid comparison operator";
        return std::nullopt;
    }
    const auto lhs = Trim(text.substr(0, position));
    const auto rhs = Trim(text.substr(position + length));
    if (lhs == "thread") {
        condition.thread_condition = true;
        condition.main_thread = rhs == "main";
        if (!condition.main_thread && !ParseInteger(rhs, condition.compared_thread)) {
            error = "thread comparison requires main or a u64 thread id";
            return std::nullopt;
        }
    } else {
        condition.left = Expression::Parse(lhs, error);
        if (!condition.left) {
            return std::nullopt;
        }
        condition.right = Expression::Parse(rhs, error);
        if (!condition.right) {
            return std::nullopt;
        }
    }
    return condition;
}

bool Condition::Evaluate(const Registers& registers, const MemoryReader& read, u64 thread_id,
                         u64 main_thread_id) const {
    u64 lhs{};
    u64 rhs{};
    if (thread_condition) {
        lhs = thread_id;
        rhs = main_thread ? main_thread_id : compared_thread;
    } else if (!left->Evaluate(registers, read, lhs) || !right->Evaluate(registers, read, rhs)) {
        return false;
    }
    switch (comparison) {
    case Comparison::Equal:
        return lhs == rhs;
    case Comparison::NotEqual:
        return lhs != rhs;
    case Comparison::Less:
        return lhs < rhs;
    case Comparison::LessEqual:
        return lhs <= rhs;
    case Comparison::Greater:
        return lhs > rhs;
    case Comparison::GreaterEqual:
        return lhs >= rhs;
    }
    return false;
}

} // namespace Core::TraceExpressions
