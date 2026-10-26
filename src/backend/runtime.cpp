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

// rt_a * rt_b -> A/X (low 16 bits), sum kept in rt_r
void Mul16(Writer& w) {
    Backend& be = *w.be;
    std::int32_t add = w.Label("mul16.add");
    std::int32_t shift = w.Label("mul16.shift");
    std::int32_t test = w.Label("mul16.test");
    w.Imm(Mn::LDA, 0);
    w.Mem(Mn::STA, be.rt_r);
    w.Mem(Mn::STA, be.rt_r, 1);
    w.Br(Mn::BEQ, test);
    w.Place(add);
    w.Op(Mn::CLC);
    w.Mem(Mn::LDA, be.rt_r);
    w.Mem(Mn::ADC, be.rt_a);
    w.Mem(Mn::STA, be.rt_r);
    w.Mem(Mn::LDA, be.rt_r, 1);
    w.Mem(Mn::ADC, be.rt_a, 1);
    w.Mem(Mn::STA, be.rt_r, 1);
    w.Place(shift);
    w.Mem(Mn::ASL, be.rt_a);
    w.Mem(Mn::ROL, be.rt_a, 1);
    w.Place(test);
    w.Mem(Mn::LSR, be.rt_b, 1);
    w.Mem(Mn::ROR, be.rt_b);
    w.Br(Mn::BCS, add);
    w.Mem(Mn::LDA, be.rt_b);
    w.Mem(Mn::ORA, be.rt_b, 1);
    w.Br(Mn::BNE, shift);
    w.Mem(Mn::LDA, be.rt_r);
    w.Mem(Mn::LDX, be.rt_r, 1);
    w.Op(Mn::RTS);
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
    void (*bodies[kRuntimeCount])(Writer&) = {Mul8, Mul16, DivU8, DivU16, DivS8, DivS16};
    for (int r = 0; r < kRuntimeCount; r++) {
        if (!be.rt_used[r]) {
            continue;
        }
        w.Place(be.rt_sym[r]);
        bodies[r](w);
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
