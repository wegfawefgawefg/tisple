#pragma once

// Division and remainder inside the code generator: gen_div.cpp picks the way, reuses a
// pair's results and calls the runtime; gen_div_const.cpp divides by constants.

#include "backend/gen.hpp"

namespace tisple {

// log2 of a power of two, else -1
int PowerOfTwo(std::uint32_t v);

// the last division's results stay in rt_a and rt_r for a later one of the same operands
// (have_rem: rt_r holds the remainder; rem_byte: only its low byte)
void Remember(Gen& g, const Expr& e, bool have_rem, bool rem_byte);
// the remainder in A into rt_r when another division of these operands may want it
bool KeepRemainder(Gen& g, ExprId id);
// the quotient at rt_a into dst or a temporary
Opnd QuotientOut(Gen& g, const Opnd* dst);
// a remainder below 256 in A, as a 16-bit value with a zero high byte
Opnd ByteRemainderOut(Gen& g, const Opnd* dst);

// unsigned by a constant of 3 to 127, not a power of two (long division): the dividend in A,
// the result left in A
void DivSmallConst8(Gen& g, ExprId id, const Expr& e, std::uint32_t c);
// the same for a 16-bit dividend a
Opnd DivSmallConst16(Gen& g, ExprId id, const Expr& e, const Opnd& a, std::uint32_t c,
                     const Opnd* dst);
// A = A / c or A % c, unsigned, c of 128 or more and not a power of two
void DivLarge8(Gen& g, Op op, std::uint32_t c);

// signed by a constant (c the divisor's signed value): false when the runtime should do it
bool DivSignedConst8(Gen& g, ExprId id, const Expr& e, std::int32_t c);
bool DivSignedConst16(Gen& g, ExprId id, const Expr& e, std::int32_t c, const Opnd* dst, Opnd& out);

} // namespace tisple
