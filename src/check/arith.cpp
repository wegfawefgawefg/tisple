#include "check/arith.hpp"

namespace tisple {

std::uint16_t MaskToKind(TypeKind kind, std::uint32_t bits) {
    if (KindBits(kind) == 8) {
        return static_cast<std::uint16_t>(bits & 0xFF);
    }
    return static_cast<std::uint16_t>(bits & 0xFFFF);
}

std::int32_t SignedValue(TypeKind kind, std::uint16_t raw) {
    if (kind == TypeKind::I8) {
        return static_cast<std::int8_t>(static_cast<std::uint8_t>(raw));
    }
    if (kind == TypeKind::I16) {
        return static_cast<std::int16_t>(raw);
    }
    return raw;
}

namespace {

std::uint16_t ShiftRight(TypeKind kind, std::uint16_t a, std::uint32_t count) {
    std::uint32_t bits = KindBits(kind);
    if (!IsSignedKind(kind)) {
        return count >= bits ? 0 : static_cast<std::uint16_t>(a >> count);
    }
    // arithmetic: the sign bit fills in from the left
    std::int32_t value = SignedValue(kind, a);
    if (count >= bits) {
        count = bits - 1;
    }
    return MaskToKind(kind, static_cast<std::uint32_t>(value >> count));
}

std::uint16_t Divide(Op op, TypeKind kind, std::uint16_t a, std::uint16_t b) {
    if (!IsSignedKind(kind)) {
        return op == Op::Div ? static_cast<std::uint16_t>(a / b)
                             : static_cast<std::uint16_t>(a % b);
    }
    // signed: truncate toward zero, remainder takes the dividend's sign; -128 / -1 wraps
    std::int32_t x = SignedValue(kind, a);
    std::int32_t y = SignedValue(kind, b);
    std::int32_t result = op == Op::Div ? x / y : x % y;
    return MaskToKind(kind, static_cast<std::uint32_t>(result));
}

bool Compare(Op op, std::int32_t x, std::int32_t y) {
    switch (op) {
    case Op::Eq:
        return x == y;
    case Op::Ne:
        return x != y;
    case Op::Lt:
        return x < y;
    case Op::Le:
        return x <= y;
    case Op::Gt:
        return x > y;
    case Op::Ge:
        return x >= y;
    default:
        return false;
    }
}

} // namespace

std::uint16_t EvalBinaryOp(Op op, TypeKind kind, std::uint16_t a, std::uint16_t b,
                           bool& div_by_zero) {
    div_by_zero = false;
    switch (op) {
    case Op::Add:
        return MaskToKind(kind, static_cast<std::uint32_t>(a) + b);
    case Op::Sub:
        return MaskToKind(kind, static_cast<std::uint32_t>(a) - b);
    case Op::Mul:
        return MaskToKind(kind, static_cast<std::uint32_t>(a) * b);
    case Op::Div:
    case Op::Mod:
        if (b == 0) {
            div_by_zero = true;
            return 0;
        }
        return Divide(op, kind, a, b);
    case Op::And:
        return static_cast<std::uint16_t>(a & b);
    case Op::Or:
        return static_cast<std::uint16_t>(a | b);
    case Op::Xor:
        return static_cast<std::uint16_t>(a ^ b);
    case Op::Shl:
        // the count is read as unsigned bits; shifting by the width or more gives 0
        return b >= KindBits(kind) ? 0 : MaskToKind(kind, static_cast<std::uint32_t>(a) << b);
    case Op::Shr:
        return ShiftRight(kind, a, b);
    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
        if (IsSignedKind(kind)) {
            return Compare(op, SignedValue(kind, a), SignedValue(kind, b)) ? 1 : 0;
        }
        return Compare(op, a, b) ? 1 : 0;
    case Op::BitNot:
    case Op::Not:
        return 0;
    }
    return 0;
}

std::uint16_t EvalUnaryOp(Op op, TypeKind kind, std::uint16_t a) {
    if (op == Op::Not) {
        return a == 0 ? 1 : 0;
    }
    return MaskToKind(kind, static_cast<std::uint32_t>(~a));
}

std::uint16_t ConvertBits(TypeKind from, TypeKind to, std::uint16_t raw) {
    std::int32_t value = SignedValue(from, raw);
    return MaskToKind(to, static_cast<std::uint32_t>(value));
}

} // namespace tisple
