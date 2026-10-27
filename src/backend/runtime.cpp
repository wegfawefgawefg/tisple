#include "backend/runtime.hpp"

namespace tisple {

namespace {

struct Writer {
    Backend* be = nullptr;
    std::vector<AsmItem>* out = nullptr;

    void Op(Mn mn, Mode mode = Mode::Imp, AsmValue value = AsmValue()) {
        out->push_back(MakeIns(mn, mode, value));
    }
    void Mem(Mn mn, std::int32_t sym, std::int32_t offset = 0) {
        Op(mn, Mode::Abs, SymValue(sym, offset));
    }
    void Imm(Mn mn, std::int32_t value) {
        Op(mn, Mode::Imm, Num(value));
    }
    void Br(Mn mn, std::int32_t label) {
        Op(mn, Mode::Rel, SymValue(label));
    }
    std::int32_t Label(const std::string& name) {
        return NewSym(*be, name, SymRole::Label, -1);
    }
    void Place(std::int32_t label) {
        out->push_back(MakeLabel(label));
    }
    // 16-bit negate in place
    void Negate16(std::int32_t sym) {
        Imm(Mn::LDA, 0);
        Op(Mn::SEC);
        Mem(Mn::SBC, sym);
        Mem(Mn::STA, sym);
        Imm(Mn::LDA, 0);
        Mem(Mn::SBC, sym, 1);
        Mem(Mn::STA, sym, 1);
    }
    void Negate8A() {
        Imm(Mn::EOR, 0xFF);
        Op(Mn::CLC);
        Imm(Mn::ADC, 1);
    }
};

// A * rt_b -> A: add a shifted multiplicand for every multiplier bit, stop when none left
void Mul8(Writer& w) {
    Backend& be = *w.be;
    std::int32_t add = w.Label("mul8.add");
    std::int32_t shift = w.Label("mul8.shift");
    std::int32_t test = w.Label("mul8.test");
    w.Mem(Mn::STA, be.rt_a);
    w.Imm(Mn::LDA, 0);
    w.Br(Mn::BEQ, test);
    w.Place(add);
    w.Op(Mn::CLC);
    w.Mem(Mn::ADC, be.rt_a);
    w.Place(shift);
    w.Mem(Mn::ASL, be.rt_a);
    w.Place(test);
    w.Mem(Mn::LSR, be.rt_b);
    w.Br(Mn::BCS, add);
    w.Br(Mn::BNE, shift);
    w.Op(Mn::RTS);
}

// the classic shift-and-add for a byte times a byte, unrolled: the multiplier's bits leave
// at the bottom of mult as the product's low byte comes in at the top, and the high byte
// builds up in A; mcand is added for each set bit. A ends with the high byte, mult with the
// low byte
void Mul8x8Steps(Writer& w, AsmValue mcand, AsmValue mult, const std::string& name) {
    w.Imm(Mn::LDA, 0);
    w.Op(Mn::LSR, Mode::Abs, mult);
    for (int bit = 0; bit < 8; bit++) {
        std::int32_t skip = w.Label(name + ".skip" + std::to_string(bit));
        w.Br(Mn::BCC, skip);
        w.Op(Mn::CLC);
        w.Op(Mn::ADC, Mode::Abs, mcand);
        w.Place(skip);
        w.Op(Mn::ROR, Mode::Acc);
        w.Op(Mn::ROR, Mode::Abs, mult);
    }
}

// A * rt_b -> A (low), X (high): both bytes, the whole 16-bit product
void Mul8x8(Writer& w) {
    Backend& be = *w.be;
    w.Mem(Mn::STA, be.rt_a);
    Mul8x8Steps(w, SymValue(be.rt_a), SymValue(be.rt_b), "mul8x8");
    w.Op(Mn::TAX);
    w.Mem(Mn::LDA, be.rt_b);
    w.Op(Mn::RTS);
}

// A += m * n (8 bits), the carry clear: m shifts left, n right, until n has no bits left; m
// and n are lost
void MulLowSteps(Writer& w, AsmValue m, AsmValue n, const std::string& name) {
    std::int32_t add = w.Label(name + ".add");
    std::int32_t shift = w.Label(name + ".shift");
    std::int32_t test = w.Label(name + ".test");
    w.Br(Mn::BCC, test);
    w.Place(add);
    w.Op(Mn::CLC);
    w.Op(Mn::ADC, Mode::Abs, m);
    w.Place(shift);
    w.Op(Mn::ASL, Mode::Abs, m);
    w.Place(test);
    w.Op(Mn::LSR, Mode::Abs, n);
    w.Br(Mn::BCS, add);
    w.Br(Mn::BNE, shift);
}

// rt_a * rt_b -> A/X, the low 16 bits: a0 * b0 is the whole product of the low bytes, and
// a1 * b0 + a0 * b1 only reaches the high byte, so it is two byte multiplies kept to 8 bits,
// each skipped when a high byte is zero and ending when its multiplier runs out
void Mul16(Writer& w) {
    Backend& be = *w.be;
    AsmValue m = SymValue(be.rt_r);
    AsmValue n = SymValue(be.rt_r, 1);
    std::int32_t second = w.Label("mul16.second");
    std::int32_t low = w.Label("mul16.low");
    // rt_s = a1 * b0 + a0 * b1, mod 256
    w.Imm(Mn::LDA, 0);
    w.Mem(Mn::STA, be.rt_s);
    w.Mem(Mn::LDA, be.rt_a, 1);
    w.Br(Mn::BEQ, second);
    w.Op(Mn::STA, Mode::Abs, m);
    w.Mem(Mn::LDA, be.rt_b);
    w.Op(Mn::STA, Mode::Abs, n);
    w.Imm(Mn::LDA, 0);
    w.Op(Mn::CLC);
    MulLowSteps(w, m, n, "mul16.hi1");
    w.Mem(Mn::STA, be.rt_s);
    w.Place(second);
    w.Mem(Mn::LDA, be.rt_b, 1);
    w.Br(Mn::BEQ, low);
    w.Op(Mn::STA, Mode::Abs, n);
    w.Mem(Mn::LDA, be.rt_a);
    w.Op(Mn::STA, Mode::Abs, m);
    w.Mem(Mn::LDA, be.rt_s);
    w.Op(Mn::CLC);
    MulLowSteps(w, m, n, "mul16.hi2");
    w.Mem(Mn::STA, be.rt_s);
    // a0 * b0, its high byte plus rt_s
    w.Place(low);
    Mul8x8Steps(w, SymValue(be.rt_a), SymValue(be.rt_b), "mul16.lo");
    w.Op(Mn::CLC);
    w.Mem(Mn::ADC, be.rt_s);
    w.Op(Mn::TAX);
    w.Mem(Mn::LDA, be.rt_b);
    w.Op(Mn::RTS);
}

// x * y for bytes by quarter squares: x * y = sq(x + y) - sq(|x - y|) with sq(n) the
// table's n * n / 4, rounded down, which is exact since x + y and x - y are both odd or both
// even. Y = |x - y|, X = (x + y) & $FF, and the carry its ninth bit, which picks the half of
// the table x + y reads. a_holds_x: A already holds x
void QsIndices(Writer& w, AsmValue x, AsmValue y, bool a_holds_x, const std::string& name) {
    std::int32_t pos = w.Label(name + ".pos");
    if (!a_holds_x) {
        w.Op(Mn::LDA, Mode::Abs, x);
    }
    w.Op(Mn::SEC);
    w.Op(Mn::SBC, Mode::Abs, y);
    w.Br(Mn::BCS, pos);
    w.Imm(Mn::EOR, 0xFF);
    w.Imm(Mn::ADC, 1); // the carry is clear: this negates
    w.Place(pos);
    w.Op(Mn::TAY);
    w.Op(Mn::LDA, Mode::Abs, x);
    w.Op(Mn::CLC);
    w.Op(Mn::ADC, Mode::Abs, y);
    w.Op(Mn::TAX);
}

// a table byte: part 0 low bytes, 1 high bytes; half 1 is n of 256 and up
AsmValue SqTable(const Backend& be, int part, int half) {
    return SymValue(be.sq_tables, 256 * part + 512 * half);
}

// A = sq(x + y) - sq(|x - y|) low byte, for either half: the indices from QsIndices
void QsLowByte(Writer& w, int half) {
    Backend& be = *w.be;
    w.Op(Mn::LDA, Mode::AbsX, SqTable(be, 0, half));
    w.Op(Mn::SEC);
    w.Op(Mn::SBC, Mode::AbsY, SqTable(be, 0, 0));
}

// A = (x * y) & $FF
void QsLow(Writer& w, AsmValue x, AsmValue y, bool a_holds_x, const std::string& name) {
    std::int32_t big = w.Label(name + ".big");
    std::int32_t done = w.Label(name + ".done");
    QsIndices(w, x, y, a_holds_x, name);
    w.Br(Mn::BCS, big);
    QsLowByte(w, 0);
    w.Op(Mn::JMP, Mode::Abs, SymValue(done));
    w.Place(big);
    QsLowByte(w, 1);
    w.Place(done);
}

// the whole product, the indices from QsIndices: low byte to `low`, high byte in A
void QsWord(Writer& w, int half, AsmValue low) {
    Backend& be = *w.be;
    QsLowByte(w, half);
    w.Op(Mn::STA, Mode::Abs, low);
    w.Op(Mn::LDA, Mode::AbsX, SqTable(be, 1, half));
    w.Op(Mn::SBC, Mode::AbsY, SqTable(be, 1, 0));
}

// A * rt_b -> A, 8 bits
void Mul8Q(Writer& w) {
    Backend& be = *w.be;
    std::int32_t big = w.Label("mul8q.big");
    w.Mem(Mn::STA, be.rt_a);
    QsIndices(w, SymValue(be.rt_a), SymValue(be.rt_b), true, "mul8q");
    w.Br(Mn::BCS, big);
    QsLowByte(w, 0);
    w.Op(Mn::RTS);
    w.Place(big);
    QsLowByte(w, 1);
    w.Op(Mn::RTS);
}

// A * rt_b -> A (low), X (high)
void Mul8x8Q(Writer& w) {
    Backend& be = *w.be;
    std::int32_t big = w.Label("mul8x8q.big");
    w.Mem(Mn::STA, be.rt_a);
    QsIndices(w, SymValue(be.rt_a), SymValue(be.rt_b), true, "mul8x8q");
    w.Br(Mn::BCS, big);
    for (int half = 0; half < 2; half++) {
        if (half == 1) {
            w.Place(big);
        }
        QsWord(w, half, SymValue(be.rt_s));
        w.Op(Mn::TAX);
        w.Mem(Mn::LDA, be.rt_s);
        w.Op(Mn::RTS);
    }
}

// rt_a * rt_b -> A/X, the low 16 bits, as Mul16 splits it
void Mul16Q(Writer& w) {
    Backend& be = *w.be;
    std::int32_t second = w.Label("mul16q.second");
    std::int32_t low = w.Label("mul16q.low");
    std::int32_t big = w.Label("mul16q.big");
    // rt_s = a1 * b0 + a0 * b1, mod 256
    w.Imm(Mn::LDA, 0);
    w.Mem(Mn::STA, be.rt_s);
    w.Mem(Mn::LDA, be.rt_a, 1);
    w.Br(Mn::BEQ, second);
    QsLow(w, SymValue(be.rt_a, 1), SymValue(be.rt_b), true, "mul16q.hi1");
    w.Mem(Mn::STA, be.rt_s);
    w.Place(second);
    w.Mem(Mn::LDA, be.rt_b, 1);
    w.Br(Mn::BEQ, low);
    QsLow(w, SymValue(be.rt_b, 1), SymValue(be.rt_a), true, "mul16q.hi2");
    w.Op(Mn::CLC);
    w.Mem(Mn::ADC, be.rt_s);
    w.Mem(Mn::STA, be.rt_s);
    // a0 * b0, its high byte plus rt_s
    w.Place(low);
    QsIndices(w, SymValue(be.rt_a), SymValue(be.rt_b), false, "mul16q.lo");
    w.Br(Mn::BCS, big);
    for (int half = 0; half < 2; half++) {
        if (half == 1) {
            w.Place(big);
        }
        QsWord(w, half, SymValue(be.rt_r));
        w.Op(Mn::CLC);
        w.Mem(Mn::ADC, be.rt_s);
        w.Op(Mn::TAX);
        w.Mem(Mn::LDA, be.rt_r);
        w.Op(Mn::RTS);
    }
}

// rt_a / rt_b -> A (and rt_a), remainder in rt_r; restoring division, one bit a step
void DivU8(Writer& w) {
    Backend& be = *w.be;
    std::int32_t loop = w.Label("divu8.loop");
    std::int32_t sub = w.Label("divu8.sub");
    std::int32_t skip = w.Label("divu8.skip");
    w.Imm(Mn::LDA, 0);
    w.Imm(Mn::LDX, 8);
    w.Place(loop);
    w.Mem(Mn::ASL, be.rt_a);
    w.Op(Mn::ROL, Mode::Acc);
    w.Br(Mn::BCS, sub); // the remainder passed 255: certainly at least the divisor
    w.Mem(Mn::CMP, be.rt_b);
    w.Br(Mn::BCC, skip);
    w.Place(sub);
    w.Mem(Mn::SBC, be.rt_b);
    w.Mem(Mn::INC, be.rt_a);
    w.Place(skip);
    w.Op(Mn::DEX);
    w.Br(Mn::BNE, loop);
    w.Mem(Mn::STA, be.rt_r);
    w.Mem(Mn::LDA, be.rt_a);
    w.Op(Mn::RTS);
}

// rt_a / rt_b -> rt_a, remainder in rt_r
void DivU16(Writer& w) {
    Backend& be = *w.be;
    std::int32_t loop = w.Label("divu16.loop");
    std::int32_t sub = w.Label("divu16.sub");
    std::int32_t skip = w.Label("divu16.skip");
    w.Imm(Mn::LDA, 0);
    w.Mem(Mn::STA, be.rt_r);
    w.Mem(Mn::STA, be.rt_r, 1);
    w.Imm(Mn::LDX, 16);
    w.Place(loop);
    w.Mem(Mn::ASL, be.rt_a);
    w.Mem(Mn::ROL, be.rt_a, 1);
    w.Mem(Mn::ROL, be.rt_r);
    w.Mem(Mn::ROL, be.rt_r, 1);
    w.Br(Mn::BCS, sub);
    w.Mem(Mn::LDA, be.rt_r);
    w.Mem(Mn::CMP, be.rt_b);
    w.Mem(Mn::LDA, be.rt_r, 1);
    w.Mem(Mn::SBC, be.rt_b, 1);
    w.Br(Mn::BCC, skip);
    w.Place(sub);
    w.Mem(Mn::LDA, be.rt_r);
    w.Op(Mn::SEC);
    w.Mem(Mn::SBC, be.rt_b);
    w.Mem(Mn::STA, be.rt_r);
    w.Mem(Mn::LDA, be.rt_r, 1);
    w.Mem(Mn::SBC, be.rt_b, 1);
    w.Mem(Mn::STA, be.rt_r, 1);
    w.Mem(Mn::INC, be.rt_a);
    w.Place(skip);
    w.Op(Mn::DEX);
    w.Br(Mn::BNE, loop);
    w.Op(Mn::RTS);
}

// signed: divide the magnitudes; the quotient is negative when the signs differ, the
// remainder takes the dividend's sign. rt_s: sign of the quotient, rt_s+1: of the dividend
void DivS8(Writer& w) {
    Backend& be = *w.be;
    std::int32_t a_pos = w.Label("divs8.apos");
    std::int32_t b_pos = w.Label("divs8.bpos");
    std::int32_t q_pos = w.Label("divs8.qpos");
    std::int32_t r_pos = w.Label("divs8.rpos");
    w.Mem(Mn::LDA, be.rt_a);
    w.Mem(Mn::STA, be.rt_s, 1);
    w.Mem(Mn::EOR, be.rt_b);
    w.Mem(Mn::STA, be.rt_s);
    w.Mem(Mn::LDA, be.rt_a);
    w.Br(Mn::BPL, a_pos);
    w.Negate8A();
    w.Mem(Mn::STA, be.rt_a);
    w.Place(a_pos);
    w.Mem(Mn::LDA, be.rt_b);
    w.Br(Mn::BPL, b_pos);
    w.Negate8A();
    w.Mem(Mn::STA, be.rt_b);
    w.Place(b_pos);
    w.Mem(Mn::JSR, be.rt_sym[static_cast<int>(Runtime::DivU8)]);
    w.Mem(Mn::BIT, be.rt_s);
    w.Br(Mn::BPL, q_pos);
    w.Negate8A();
    w.Place(q_pos);
    w.Mem(Mn::STA, be.rt_a);
    w.Mem(Mn::BIT, be.rt_s, 1);
    w.Br(Mn::BPL, r_pos);
    w.Mem(Mn::LDA, be.rt_r);
    w.Negate8A();
    w.Mem(Mn::STA, be.rt_r);
    w.Place(r_pos);
    w.Mem(Mn::LDA, be.rt_a);
    w.Op(Mn::RTS);
}

void DivS16(Writer& w) {
    Backend& be = *w.be;
    std::int32_t a_pos = w.Label("divs16.apos");
    std::int32_t b_pos = w.Label("divs16.bpos");
    std::int32_t q_pos = w.Label("divs16.qpos");
    std::int32_t r_pos = w.Label("divs16.rpos");
    w.Mem(Mn::LDA, be.rt_a, 1);
    w.Mem(Mn::STA, be.rt_s, 1);
    w.Mem(Mn::EOR, be.rt_b, 1);
    w.Mem(Mn::STA, be.rt_s);
    w.Mem(Mn::BIT, be.rt_s, 1);
    w.Br(Mn::BPL, a_pos);
    w.Negate16(be.rt_a);
    w.Place(a_pos);
    w.Mem(Mn::LDA, be.rt_b, 1);
    w.Br(Mn::BPL, b_pos);
    w.Negate16(be.rt_b);
    w.Place(b_pos);
    w.Mem(Mn::JSR, be.rt_sym[static_cast<int>(Runtime::DivU16)]);
    w.Mem(Mn::BIT, be.rt_s);
    w.Br(Mn::BPL, q_pos);
    w.Negate16(be.rt_a);
    w.Place(q_pos);
    w.Mem(Mn::BIT, be.rt_s, 1);
    w.Br(Mn::BPL, r_pos);
    w.Negate16(be.rt_r);
    w.Place(r_pos);
    w.Op(Mn::RTS);
}

} // namespace

void EmitRuntime(Backend& be, std::vector<AsmItem>& out) {
    if (be.rt_used[static_cast<int>(Runtime::DivS8)]) {
        be.rt_used[static_cast<int>(Runtime::DivU8)] = true;
    }
    if (be.rt_used[static_cast<int>(Runtime::DivS16)]) {
        be.rt_used[static_cast<int>(Runtime::DivU16)] = true;
    }
    Writer w;
    w.be = &be;
    w.out = &out;
    void (*bodies[kRuntimeCount])(Writer&) = {Mul8,   Mul16,  DivU8, DivU16, DivS8,
                                              DivS16, Mul8x8, Mul8Q, Mul16Q, Mul8x8Q};
    for (int r = 0; r < kRuntimeCount; r++) {
        if (!be.rt_used[r]) {
            continue;
        }
        w.Place(be.rt_sym[r]);
        bodies[r](w);
    }
}

void EmitRuntimeTables(Backend& be, std::vector<AsmItem>& out) {
    bool quarter = be.rt_used[static_cast<int>(Runtime::Mul8Q)] ||
                   be.rt_used[static_cast<int>(Runtime::Mul16Q)] ||
                   be.rt_used[static_cast<int>(Runtime::Mul8x8Q)];
    if (!quarter) {
        return;
    }
    out.push_back(MakeLabel(be.sq_tables));
    for (int half = 0; half < 2; half++) {
        for (int part = 0; part < 2; part++) {
            for (std::uint32_t i = 0; i < 256; i++) {
                std::uint32_t n = 256 * static_cast<std::uint32_t>(half) + i;
                std::uint32_t sq = n * n / 4;
                std::uint32_t byte = part == 0 ? sq & 0xFF : (sq >> 8) & 0xFF;
                out.push_back(MakeByte(Num(static_cast<std::int32_t>(byte))));
            }
        }
    }
}

void EmitStartup(Backend& be, const StartupPlan& plan, std::vector<AsmItem>& out) {
    Writer w;
    w.be = &be;
    w.out = &out;
    std::int32_t save = w.Label("start.save");
    std::int32_t restore = w.Label("start.restore");
    w.Op(Mn::CLD);
    w.Imm(Mn::LDX, 0);
    w.Place(save);
    w.Op(Mn::LDA, Mode::ZpX, Num(0));
    w.Op(Mn::STA, Mode::AbsX, SymValue(plan.save_sym));
    w.Op(Mn::INX);
    w.Br(Mn::BNE, save);
    if (plan.zp_count > 0) {
        std::int32_t init = w.Label("start.zp");
        w.Imm(Mn::LDX, static_cast<std::int32_t>(plan.zp_count));
        w.Place(init);
        w.Op(Mn::LDA, Mode::AbsX, SymValue(plan.image_sym, -1));
        w.Op(Mn::STA, Mode::AbsX, Num(static_cast<std::int32_t>(plan.zp_base) - 1));
        w.Op(Mn::DEX);
        w.Br(Mn::BNE, init);
    }
    for (const auto& store : plan.stores) {
        w.Imm(Mn::LDA, store.second);
        w.Op(Mn::STA, Mode::Abs, Num(store.first));
    }
    w.Place(plan.ready_sym);
    if (plan.main_sym >= 0) {
        w.Mem(Mn::JSR, plan.main_sym);
    }
    w.Place(plan.after_sym);
    w.Imm(Mn::LDX, 0);
    w.Place(restore);
    w.Op(Mn::LDA, Mode::AbsX, SymValue(plan.save_sym));
    w.Op(Mn::STA, Mode::ZpX, Num(0));
    w.Op(Mn::INX);
    w.Br(Mn::BNE, restore);
    w.Op(Mn::RTS);
}

} // namespace tisple
