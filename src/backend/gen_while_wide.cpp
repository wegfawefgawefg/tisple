#include "backend/gen.hpp"

#include <algorithm>

// A u16 variable of a while loop in Y and memory (gen_while.cpp), like a u16 for counter
// (gen_loop.cpp): low byte in Y, high byte in its own memory, and every array or pointer it
// indexes read through a derived pointer, base + (high << 8), set before the loop. Each of
// its sets moves the pointers' high bytes with its own; the code for that is put in once the
// body is done and the pointers are known (WideFixup).

namespace tisple {

namespace {

const Expr& At(const Gen& g, ExprId id) {
    return ExprOf(*g.program, id);
}

// the other operand of var + e, e + var or var - e; -1 if value is not one
ExprId AddendOf(const Gen& g, std::int32_t var, ExprId value, bool& sub) {
    const Expr& v = At(g, value);
    if (v.kind != ExprKind::Binary || (v.op != Op::Add && v.op != Op::Sub)) {
        return -1;
    }
    auto is_var = [&](ExprId id) {
        const Expr& x = At(g, id);
        return x.kind == ExprKind::VarRef && x.ref == var;
    };
    sub = v.op == Op::Sub;
    if (is_var(v.a)) {
        return v.b;
    }
    return !sub && is_var(v.b) ? v.a : -1;
}

bool IsPlain(const Opnd& o) {
    if (o.in_a || o.size == 0) {
        return false;
    }
    for (int k = 0; k < o.size; k++) {
        if (o.b[k].kind != ByteKind::Imm && o.b[k].kind != ByteKind::Mem) {
            return false;
        }
    }
    return true;
}

void AddFixup(Gen& g, std::int32_t label, WideFix fix) {
    WideFixup f;
    f.label = label;
    f.fix = fix;
    g.wide_fixups.push_back(f);
}

} // namespace

// value is var + c (mod 2^16) for a constant c
bool StepOf(const Gen& g, std::int32_t var, ExprId value, std::uint16_t& c) {
    const Expr& v = At(g, value);
    if (v.kind != ExprKind::Binary || (v.op != Op::Add && v.op != Op::Sub)) {
        return false;
    }
    auto is_var = [&](ExprId id) {
        const Expr& x = At(g, id);
        return x.kind == ExprKind::VarRef && x.ref == var;
    };
    if (is_var(v.a) && ConstOf(g, v.b, c)) {
        c = v.op == Op::Sub ? static_cast<std::uint16_t>(-c) : c;
        return true;
    }
    return v.op == Op::Add && is_var(v.b) && ConstOf(g, v.a, c);
}

// (set var value) with var's low byte in Y and its high byte at `high`
void SetWide(Gen& g, ExprId value) {
    std::int32_t var = g.wide->var;
    AsmValue high = g.wide->high;
    std::int32_t skip = NewLabel(g);
    std::uint16_t c = 0;
    if (StepOf(g, var, value, c)) {
        if (c == 0) {
            return;
        }
        if (c == 0xFFFF) {
            // the high byte moves when the low byte goes from 0 to $FF
            Emit(g, Mn::CPY, Mode::Imm, Num(0));
            Branch(g, Mn::BNE, skip);
            Emit(g, Mn::DEC, Mode::Abs, high);
            AddFixup(g, skip, WideFix::Dec);
            PlaceLabel(g, skip);
            EmitPinned(g, Mn::DEY);
            return;
        }
        if (c == 1) {
            EmitPinned(g, Mn::INY);
            Branch(g, Mn::BNE, skip);
            Emit(g, Mn::INC, Mode::Abs, high);
            AddFixup(g, skip, WideFix::Inc);
        } else if (c < 0x100) {
            Emit(g, Mn::TYA, Mode::Imp);
            Emit(g, Mn::CLC, Mode::Imp);
            Emit(g, Mn::ADC, Mode::Imm, Num(c));
            EmitPinned(g, Mn::TAY);
            Branch(g, Mn::BCC, skip);
            Emit(g, Mn::INC, Mode::Abs, high);
            AddFixup(g, skip, WideFix::Inc);
        } else if (c > 0xFF00) {
            Emit(g, Mn::TYA, Mode::Imp);
            Emit(g, Mn::SEC, Mode::Imp);
            Emit(g, Mn::SBC, Mode::Imm, Num((0x10000 - c) & 0xFF));
            EmitPinned(g, Mn::TAY);
            Branch(g, Mn::BCS, skip);
            Emit(g, Mn::DEC, Mode::Abs, high);
            AddFixup(g, skip, WideFix::Dec);
        } else {
            Emit(g, Mn::TYA, Mode::Imp);
            Emit(g, Mn::CLC, Mode::Imp);
            Emit(g, Mn::ADC, Mode::Imm, Num(c & 0xFF));
            EmitPinned(g, Mn::TAY);
            Emit(g, Mn::LDA, Mode::Abs, high);
            Emit(g, Mn::ADC, Mode::Imm, Num(c >> 8));
            Emit(g, Mn::STA, Mode::Abs, high);
            AddFixup(g, skip, WideFix::Compute);
        }
        PlaceLabel(g, skip);
        return;
    }
    bool sub = false;
    ExprId addend = AddendOf(g, var, value, sub);
    if (addend >= 0 && !HasEffects(*g.program, addend)) {
        // both bytes of the addend are read around the TAY: keep them where Y cannot move them
        Opnd o = Eval(g, addend, nullptr);
        if (!IsPlain(o)) {
            o = Snapshot(g, o);
            if (!IsPlain(o)) {
                Opnd t = NewTemp(g, 2);
                CopyOpnd(g, o, t);
                o = t;
            }
        }
        Emit(g, Mn::TYA, Mode::Imp);
        Emit(g, sub ? Mn::SEC : Mn::CLC, Mode::Imp);
        OpA(g, sub ? Mn::SBC : Mn::ADC, o, 0);
        EmitPinned(g, Mn::TAY);
        Emit(g, Mn::LDA, Mode::Abs, high);
        OpA(g, sub ? Mn::SBC : Mn::ADC, o, 1);
        Emit(g, Mn::STA, Mode::Abs, high);
        AddFixup(g, skip, WideFix::Compute);
        PlaceLabel(g, skip);
        return;
    }
    Opnd t = NewTemp(g, 2);
    EvalInto(g, value, t);
    LoadA(g, t, 0);
    EmitPinned(g, Mn::TAY);
    LoadA(g, t, 1);
    Emit(g, Mn::STA, Mode::Abs, high);
    AddFixup(g, skip, WideFix::Compute);
    PlaceLabel(g, skip);
}

// each fixup's code, moved to just before its label (items from `first` on)
void ApplyFixups(Gen& g, const std::vector<Derived>& used, const AsmValue& high,
                 std::size_t first) {
    std::vector<WideFixup> fixups = g.wide_fixups;
    g.wide_fixups.clear();
    if (used.empty()) {
        return;
    }
    for (const WideFixup& f : fixups) {
        std::size_t at = g.out->size();
        for (std::size_t i = first; i < g.out->size(); i++) {
            const AsmItem& item = (*g.out)[i];
            if (item.kind == ItemKind::Label && item.value.sym == f.label) {
                at = i;
                break;
            }
        }
        if (at == g.out->size()) {
            continue;
        }
        std::size_t before = g.out->size();
        FollowHigh(g, used, f.fix, high);
        std::rotate(g.out->begin() + static_cast<std::ptrdiff_t>(at),
                    g.out->begin() + static_cast<std::ptrdiff_t>(before), g.out->end());
    }
}

// when the loop only steps the high byte (INC or DEC, for steps of 1) and reads it nowhere,
// and a derived pointer with a constant base moves with it: those steps go, and the high byte
// is worked out from that pointer after the loop (the returned pointer); else nullptr
const Derived* DropHighSteps(Gen& g, const std::vector<Derived>& used, const AsmValue& high,
                             std::size_t first) {
    const Derived* rebuild = nullptr;
    for (const Derived& d : used) {
        if (d.var < 0 && !rebuild) {
            rebuild = &d;
        }
    }
    if (!rebuild) {
        return nullptr;
    }
    std::vector<std::size_t> steps;
    for (std::size_t i = first; i < g.out->size(); i++) {
        const AsmItem& item = (*g.out)[i];
        if (item.kind != ItemKind::Ins || item.value.sym != high.sym ||
            item.value.offset != high.offset || item.mode == Mode::Rel) {
            continue;
        }
        if ((item.mn != Mn::INC && item.mn != Mn::DEC) || item.mode != Mode::Abs) {
            return nullptr;
        }
        steps.push_back(i);
    }
    for (std::size_t k = steps.size(); k-- > 0;) {
        g.out->erase(g.out->begin() + static_cast<std::ptrdiff_t>(steps[k]));
    }
    return rebuild;
}

// the body ends with (set var (+ var 1)): with the test at the top, its BNE over the page
// step goes straight back to the test
bool EndsWithUnitStep(const Gen& g, ExprId body, std::int32_t var) {
    const Expr& b = At(g, body);
    if (b.kind != ExprKind::Do || b.count == 0) {
        return false;
    }
    const Expr& last = At(g, ListItem(*g.program, b, b.count - 1));
    if (last.kind != ExprKind::Set) {
        return false;
    }
    const Expr& place = At(g, last.a);
    std::uint16_t c = 0;
    return place.kind == ExprKind::VarRef && place.ref == var && StepOf(g, var, last.b, c) &&
           c == 1;
}

} // namespace tisple
