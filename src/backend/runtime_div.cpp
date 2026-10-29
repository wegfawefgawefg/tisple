// Division by a variable: the runtime routines (license as runtime.hpp: textbook shift and
// subtract, written for Tisple).
//
// Most divisions in the random race need no loop at all, so each routine first takes the
// cases that are one compare: a dividend below the divisor (quotient 0), a divisor of 0 or 1,
// a divisor whose top bit is set (quotient 0 or 1). What is left divides by doubling the
// divisor while it stays at most half the dividend, then one compare and subtract per
// quotient bit, so the work is the quotient's length rather than the operands' width; a byte
// quotient of 16 or more (doubling would cost more than it saves) takes the plain eight
// steps instead. A 16-bit dividend by a byte divisor is two byte divisions, high byte first,
// its remainder staying in A. No path is slower than the plain loop it replaces.
//
// Division by zero gives what the restoring division always gave (docs/language.md):
// every quotient bit set, the dividend as the remainder; signed, the magnitudes divide and
// the signs are fixed after, so -5 / 0 is 1 and -5 % 0 is -5.

#include "backend/runtime_writer.hpp"

namespace tisple {

namespace {

// 8 long-division steps on the byte at q, the remainder so far in A (below the byte divisor
// at rt_b): q becomes the quotient byte, A the remainder. A remainder that carries out of A
// when doubled is past 255, so certainly at least the divisor: subtract, and the quotient
// bit is 1 whatever SBC's borrow says. The first ROL shifts in a bit the ninth shifts out.
// X is the count.
void ByteSteps(Writer& w, AsmValue q, const std::string& name) {
    Backend& be = *w.be;
    std::int32_t step = w.Label(name + ".step");
    std::int32_t next = w.Label(name + ".next");
    std::int32_t over = w.Label(name + ".over");
    std::int32_t done = w.Label(name + ".done");
    w.Imm(Mn::LDX, 8);
    w.Place(step);
    w.Op(Mn::ROL, Mode::Abs, q);
    w.Op(Mn::ROL, Mode::Acc);
    w.Br(Mn::BCS, over);
    w.Mem(Mn::CMP, be.rt_b);
    w.Br(Mn::BCC, next);
    w.Mem(Mn::SBC, be.rt_b);
    w.Place(next);
    w.Op(Mn::DEX);
    w.Br(Mn::BNE, step);
    w.Op(Mn::ROL, Mode::Abs, q);
    w.Jmp(done);
    w.Place(over);
    w.Mem(Mn::SBC, be.rt_b);
    w.Op(Mn::SEC);
    w.Br(Mn::BCS, next);
    w.Place(done);
}

} // namespace

// rt_a / rt_b -> A and rt_a, remainder in rt_r
void DivU8(Writer& w) {
    Backend& be = *w.be;
    std::int32_t below = w.Label("divu8.below");
    std::int32_t one = w.Label("divu8.one");
    std::int32_t tiny = w.Label("divu8.tiny");
    std::int32_t by_zero = w.Label("divu8.zero");
    std::int32_t align = w.Label("divu8.align");
    std::int32_t go = w.Label("divu8.go");
    std::int32_t step = w.Label("divu8.step");
    std::int32_t keep = w.Label("divu8.keep");
    std::int32_t small = w.Label("divu8.small");
    std::int32_t shifts = w.Label("divu8.shifts");
    std::int32_t small_step = w.Label("divu8.smallstep");
    std::int32_t small_keep = w.Label("divu8.smallkeep");
    w.Mem(Mn::LDA, be.rt_a);
    w.Mem(Mn::CMP, be.rt_b);
    w.Br(Mn::BCC, below);
    w.Place(w.divu8_ge);
    w.Mem(Mn::LDX, be.rt_b);
    w.Br(Mn::BMI, one);
    w.Imm(Mn::CPX, 2);
    w.Br(Mn::BCC, tiny);
    // a quotient of 16 or more (only with a divisor below 16): eight plain steps
    w.Imm(Mn::CPX, 16);
    w.Br(Mn::BCS, shifts);
    w.Op(Mn::LSR, Mode::Acc);
    w.Op(Mn::LSR, Mode::Acc);
    w.Op(Mn::LSR, Mode::Acc);
    w.Op(Mn::LSR, Mode::Acc);
    w.Mem(Mn::CMP, be.rt_b);
    w.Br(Mn::BCS, small);
    w.Mem(Mn::LDA, be.rt_a);
    // at most a: double it (n times, in X, at most 3) while it stays below a / 2 + 1
    w.Place(shifts);
    w.Op(Mn::LSR, Mode::Acc);
    w.Op(Mn::CLC);
    w.Imm(Mn::ADC, 1);
    w.Mem(Mn::STA, be.rt_r);
    w.Op(Mn::TXA);
    w.Imm(Mn::LDX, 0);
    w.Place(align);
    w.Mem(Mn::CMP, be.rt_r);
    w.Br(Mn::BCS, go);
    w.Op(Mn::ASL, Mode::Acc);
    w.Op(Mn::INX);
    w.Br(Mn::BNE, align);
    // n + 1 quotient bits, the divisor halving each step
    w.Place(go);
    w.Mem(Mn::STA, be.rt_b);
    w.Mem(Mn::LDA, be.rt_a);
    w.Imm(Mn::LDY, 0);
    w.Mem(Mn::STY, be.rt_a);
    w.Place(step);
    w.Mem(Mn::CMP, be.rt_b);
    w.Br(Mn::BCC, keep);
    w.Mem(Mn::SBC, be.rt_b);
    w.Place(keep);
    w.Mem(Mn::ROL, be.rt_a);
    w.Mem(Mn::LSR, be.rt_b);
    w.Op(Mn::DEX);
    w.Br(Mn::BPL, step);
    w.Mem(Mn::STA, be.rt_r);
    w.Mem(Mn::LDA, be.rt_a);
    w.Op(Mn::RTS);
    // a divisor of 2 to 15, a quotient of 16 or more: the remainder stays below 16, never
    // carrying
    w.Place(small);
    w.Imm(Mn::LDA, 0);
    w.Imm(Mn::LDX, 8);
    w.Place(small_step);
    w.Mem(Mn::ROL, be.rt_a);
    w.Op(Mn::ROL, Mode::Acc);
    w.Mem(Mn::CMP, be.rt_b);
    w.Br(Mn::BCC, small_keep);
    w.Mem(Mn::SBC, be.rt_b);
    w.Place(small_keep);
    w.Op(Mn::DEX);
    w.Br(Mn::BNE, small_step);
    w.Mem(Mn::ROL, be.rt_a);
    w.Mem(Mn::STA, be.rt_r);
    w.Mem(Mn::LDA, be.rt_a);
    w.Op(Mn::RTS);
    // 128 or more: the quotient is 1 (the carry is still set from the compare)
    w.Place(one);
    w.Mem(Mn::SBC, be.rt_b);
    w.Mem(Mn::STA, be.rt_r);
    w.Imm(Mn::LDA, 1);
    w.Mem(Mn::STA, be.rt_a);
    w.Op(Mn::RTS);
    // by 1: the quotient is a, already in A and rt_a; by 0: all ones, the remainder a
    w.Place(tiny);
    w.Op(Mn::DEX);
    w.Br(Mn::BNE, by_zero);
    w.Mem(Mn::STX, be.rt_r);
    w.Op(Mn::RTS);
    w.Place(by_zero);
    w.Mem(Mn::STA, be.rt_r);
    w.Mem(Mn::STX, be.rt_a);
    w.Op(Mn::TXA);
    w.Op(Mn::RTS);
    w.Place(below);
    w.Mem(Mn::STA, be.rt_r);
    w.Imm(Mn::LDA, 0);
    w.Mem(Mn::STA, be.rt_a);
    w.Op(Mn::RTS);
}

namespace {

// divu16 when the divisor is 256 or more and at most a: the quotient is a byte. Doubles the
// divisor while it stays at most a / 2 (kept in rt_r), then a trial subtract per bit
void DivU16Wide(Writer& w) {
    Backend& be = *w.be;
    std::int32_t one = w.Label("divu16.one");
    std::int32_t align = w.Label("divu16.align");
    std::int32_t go = w.Label("divu16.go");
    std::int32_t step = w.Label("divu16.step");
    std::int32_t keep = w.Label("divu16.keep");
    w.Br(Mn::BMI, one);
    w.Mem(Mn::LDA, be.rt_a, 1);
    w.Op(Mn::LSR, Mode::Acc);
    w.Mem(Mn::STA, be.rt_r, 1);
    w.Mem(Mn::LDA, be.rt_a);
    w.Op(Mn::ROR, Mode::Acc);
    w.Mem(Mn::STA, be.rt_r);
    w.Imm(Mn::LDX, 0);
    w.Place(align);
    w.Mem(Mn::LDA, be.rt_r);
    w.Mem(Mn::CMP, be.rt_b);
    w.Mem(Mn::LDA, be.rt_r, 1);
    w.Mem(Mn::SBC, be.rt_b, 1);
    w.Br(Mn::BCC, go);
    w.Mem(Mn::ASL, be.rt_b);
    w.Mem(Mn::ROL, be.rt_b, 1);
    w.Op(Mn::INX);
    w.Br(Mn::BNE, align);
    w.Place(go);
    w.Mem(Mn::LDA, be.rt_a);
    w.Mem(Mn::STA, be.rt_r);
    w.Mem(Mn::LDA, be.rt_a, 1);
    w.Mem(Mn::STA, be.rt_r, 1);
    w.Imm(Mn::LDA, 0);
    w.Mem(Mn::STA, be.rt_a);
    w.Mem(Mn::STA, be.rt_a, 1);
    w.Place(step);
    w.Mem(Mn::LDA, be.rt_r);
    w.Op(Mn::SEC);
    w.Mem(Mn::SBC, be.rt_b);
    w.Op(Mn::TAY);
    w.Mem(Mn::LDA, be.rt_r, 1);
    w.Mem(Mn::SBC, be.rt_b, 1);
    w.Br(Mn::BCC, keep);
    w.Mem(Mn::STA, be.rt_r, 1);
    w.Mem(Mn::STY, be.rt_r);
    w.Place(keep);
    w.Mem(Mn::ROL, be.rt_a);
    w.Mem(Mn::LSR, be.rt_b, 1);
    w.Mem(Mn::ROR, be.rt_b);
    w.Op(Mn::DEX);
    w.Br(Mn::BPL, step);
    w.Op(Mn::RTS);
    // 32768 or more: the quotient is 1
    w.Place(one);
    w.Mem(Mn::LDA, be.rt_a);
    w.Op(Mn::SEC);
    w.Mem(Mn::SBC, be.rt_b);
    w.Mem(Mn::STA, be.rt_r);
    w.Mem(Mn::LDA, be.rt_a, 1);
    w.Mem(Mn::SBC, be.rt_b, 1);
    w.Mem(Mn::STA, be.rt_r, 1);
    w.Imm(Mn::LDA, 1);
    w.Mem(Mn::STA, be.rt_a);
    w.Imm(Mn::LDA, 0);
    w.Mem(Mn::STA, be.rt_a, 1);
    w.Op(Mn::RTS);
}

} // namespace

// rt_a / rt_b -> rt_a, remainder in rt_r
void DivU16(Writer& w) {
    Backend& be = *w.be;
    std::int32_t below = w.Label("divu16.below");
    std::int32_t at_least = w.Label("divu16.atleast");
    std::int32_t wide = w.Label("divu16.wide");
    std::int32_t tiny = w.Label("divu16.tiny");
    std::int32_t by_zero = w.Label("divu16.zero");
    std::int32_t big = w.Label("divu16.big");
    std::int32_t low_only = w.Label("divu16.low");
    std::int32_t low = w.Label("divu16.lowsteps");
    w.Mem(Mn::LDA, be.rt_a, 1);
    w.Mem(Mn::CMP, be.rt_b, 1);
    w.Br(Mn::BCC, below);
    w.Br(Mn::BNE, at_least);
    w.Mem(Mn::LDA, be.rt_a);
    w.Mem(Mn::CMP, be.rt_b);
    w.Br(Mn::BCC, below);
    w.Place(at_least);
    w.Place(w.divu16_ge);
    w.Mem(Mn::LDX, be.rt_b, 1);
    w.Br(Mn::BNE, wide);
    w.Mem(Mn::LDX, be.rt_b);
    w.Imm(Mn::CPX, 2);
    w.Br(Mn::BCC, tiny);
    // a byte divisor of 2 or more: the remainder is a byte. Both bytes: the byte routine
    // (the carry is set)
    w.Mem(Mn::LDA, be.rt_a, 1);
    w.Br(Mn::BNE, big);
    w.Mem(Mn::STA, be.rt_r, 1);
    w.Mem(Mn::LDA, be.rt_a);
    w.Jmp(w.divu8_ge);
    // the high byte first, unless it is below the divisor: then it is the remainder so far
    w.Place(big);
    w.Mem(Mn::CMP, be.rt_b);
    w.Br(Mn::BCC, low_only);
    w.Imm(Mn::LDA, 0);
    ByteSteps(w, SymValue(be.rt_a, 1), "divu16.high");
    w.Jmp(low);
    w.Place(low_only);
    w.Imm(Mn::LDY, 0);
    w.Mem(Mn::STY, be.rt_a, 1);
    w.Place(low);
    ByteSteps(w, SymValue(be.rt_a), "divu16.low");
    w.Mem(Mn::STA, be.rt_r);
    w.Imm(Mn::LDA, 0);
    w.Mem(Mn::STA, be.rt_r, 1);
    w.Op(Mn::RTS);
    w.Place(wide);
    DivU16Wide(w);
    // by 1: the quotient is a; by 0: all ones, the remainder a
    w.Place(tiny);
    w.Op(Mn::DEX);
    w.Br(Mn::BNE, by_zero);
    w.Mem(Mn::STX, be.rt_r);
    w.Mem(Mn::STX, be.rt_r, 1);
    w.Op(Mn::RTS);
    w.Place(by_zero);
    w.Mem(Mn::LDA, be.rt_a);
    w.Mem(Mn::STA, be.rt_r);
    w.Mem(Mn::LDA, be.rt_a, 1);
    w.Mem(Mn::STA, be.rt_r, 1);
    w.Mem(Mn::STX, be.rt_a);
    w.Mem(Mn::STX, be.rt_a, 1);
    w.Op(Mn::RTS);
    w.Place(below);
    w.Mem(Mn::LDA, be.rt_a);
    w.Mem(Mn::STA, be.rt_r);
    w.Mem(Mn::LDA, be.rt_a, 1);
    w.Mem(Mn::STA, be.rt_r, 1);
    w.Imm(Mn::LDA, 0);
    w.Mem(Mn::STA, be.rt_a);
    w.Mem(Mn::STA, be.rt_a, 1);
    w.Op(Mn::RTS);
}

// signed: divide the magnitudes; the quotient is negative when the signs differ, the
// remainder takes the dividend's sign. rt_s: the quotient's sign, rt_s+1: the dividend. A
// magnitude below the divisor's needs no division: the quotient is 0, the remainder a
void DivS8(Writer& w) {
    Backend& be = *w.be;
    std::int32_t a_pos = w.Label("divs8.apos");
    std::int32_t b_pos = w.Label("divs8.bpos");
    std::int32_t q_pos = w.Label("divs8.qpos");
    std::int32_t r_pos = w.Label("divs8.rpos");
    std::int32_t below = w.Label("divs8.below");
    w.Mem(Mn::LDA, be.rt_a);
    w.Mem(Mn::STA, be.rt_s, 1);
    w.Mem(Mn::EOR, be.rt_b);
    w.Mem(Mn::STA, be.rt_s);
    w.Mem(Mn::LDA, be.rt_b);
    w.Br(Mn::BPL, b_pos);
    w.Negate8A();
    w.Mem(Mn::STA, be.rt_b);
    w.Place(b_pos);
    w.Mem(Mn::LDA, be.rt_a);
    w.Br(Mn::BPL, a_pos);
    w.Negate8A();
    w.Mem(Mn::STA, be.rt_a);
    w.Place(a_pos);
    w.Mem(Mn::CMP, be.rt_b);
    w.Br(Mn::BCC, below);
    w.Mem(Mn::JSR, w.divu8_ge);
    w.Mem(Mn::BIT, be.rt_s);
    w.Br(Mn::BPL, q_pos);
    w.Negate8A();
    w.Mem(Mn::STA, be.rt_a);
    w.Place(q_pos);
    w.Mem(Mn::BIT, be.rt_s, 1);
    w.Br(Mn::BPL, r_pos);
    w.Mem(Mn::LDA, be.rt_r);
    w.Negate8A();
    w.Mem(Mn::STA, be.rt_r);
    w.Place(r_pos);
    w.Mem(Mn::LDA, be.rt_a);
    w.Op(Mn::RTS);
    w.Place(below);
    w.Mem(Mn::LDA, be.rt_s, 1);
    w.Mem(Mn::STA, be.rt_r);
    w.Imm(Mn::LDA, 0);
    w.Mem(Mn::STA, be.rt_a);
    w.Op(Mn::RTS);
}

// the same for words; the dividend is kept in rt_r for the early way out
void DivS16(Writer& w) {
    Backend& be = *w.be;
    std::int32_t a_pos = w.Label("divs16.apos");
    std::int32_t b_pos = w.Label("divs16.bpos");
    std::int32_t q_pos = w.Label("divs16.qpos");
    std::int32_t r_pos = w.Label("divs16.rpos");
    std::int32_t below = w.Label("divs16.below");
    std::int32_t divide = w.Label("divs16.divide");
    w.Mem(Mn::LDA, be.rt_a);
    w.Mem(Mn::STA, be.rt_r);
    w.Mem(Mn::LDA, be.rt_a, 1);
    w.Mem(Mn::STA, be.rt_r, 1);
    w.Mem(Mn::STA, be.rt_s, 1);
    w.Mem(Mn::EOR, be.rt_b, 1);
    w.Mem(Mn::STA, be.rt_s);
    w.Mem(Mn::LDA, be.rt_b, 1);
    w.Br(Mn::BPL, b_pos);
    w.Negate16(be.rt_b);
    w.Place(b_pos);
    w.Mem(Mn::BIT, be.rt_s, 1);
    w.Br(Mn::BPL, a_pos);
    w.Negate16(be.rt_a);
    w.Place(a_pos);
    w.Mem(Mn::LDA, be.rt_a, 1);
    w.Mem(Mn::CMP, be.rt_b, 1);
    w.Br(Mn::BCC, below);
    w.Br(Mn::BNE, divide);
    w.Mem(Mn::LDA, be.rt_a);
    w.Mem(Mn::CMP, be.rt_b);
    w.Br(Mn::BCC, below);
    w.Place(divide);
    w.Mem(Mn::JSR, w.divu16_ge);
    w.Mem(Mn::BIT, be.rt_s);
    w.Br(Mn::BPL, q_pos);
    w.Negate16(be.rt_a);
    w.Place(q_pos);
    w.Mem(Mn::BIT, be.rt_s, 1);
    w.Br(Mn::BPL, r_pos);
    w.Negate16(be.rt_r);
    w.Place(r_pos);
    w.Op(Mn::RTS);
    w.Place(below);
    w.Imm(Mn::LDA, 0);
    w.Mem(Mn::STA, be.rt_a);
    w.Mem(Mn::STA, be.rt_a, 1);
    w.Op(Mn::RTS);
}

} // namespace tisple
