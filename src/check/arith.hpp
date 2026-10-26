#pragma once

// target arithmetic on raw bits: shared by constant folding and the interpreter,
// so a folded constant and a computed value can never disagree

#include "check/program.hpp"

namespace tisple {

std::uint16_t MaskToKind(TypeKind kind, std::uint32_t bits);
// raw bits as a mathematical value: sign-extended for i8/i16
std::int32_t SignedValue(TypeKind kind, std::uint16_t raw);

// operands and result are raw bits of `kind`; comparisons give 0 or 1.
// div_by_zero is set for / and % by zero; the result is still defined, as the backend's
// routine gives it: x / 0 is all ones (-1 signed), or 1 for a negative signed x; x % 0 is x
std::uint16_t EvalBinaryOp(Op op, TypeKind kind, std::uint16_t a, std::uint16_t b,
                           bool& div_by_zero);
std::uint16_t EvalUnaryOp(Op op, TypeKind kind, std::uint16_t a);
// (u8 x) and friends: zero-extend unsigned, sign-extend signed, truncate
std::uint16_t ConvertBits(TypeKind from, TypeKind to, std::uint16_t raw);

} // namespace tisple
