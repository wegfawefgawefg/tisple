#include "backend/gen.hpp"

#include <algorithm>

// Step pointers. A u16 variable that a loop changes only by constant steps, (set k (+ k 1))
// in a loop's body or a u16 for counter kept in memory, indexes its arrays and pointers
// through a pointer per base: ptr = base + offset + k, worked out once before the loop and
// moved by the same constant wherever k is stepped. (at flags k) is then LDY #0 / LDA (ptr),Y
// instead of a 16-bit add every time. The outermost loop that only steps k owns the
// pointers, so a step in an inner loop moves them too.
//
// A step is noted where its code ends (NoteStep), with a label; the code moving the pointers
// goes in just before it once the loop is done and its pointers are known, as for wide while
// loops (gen_while.cpp).

namespace tisple {

namespace {

constexpr int kMaxDepth = 64;

const Expr& At(const Gen& g, ExprId id) {
    return ExprOf(*g.program, id);
}

struct StepUse {
    int steps = 0;
    bool other = false; // written some other way, or declared inside
};

// value is var plus the constant step (mod 2^16): (+ var c), (+ c var), (- var c)
bool ConstStep(const Program& program, std::int32_t var, ExprId value, std::uint16_t& step) {
    const Expr& v = ExprOf(program, value);
    if (v.kind != ExprKind::Binary || (v.op != Op::Add && v.op != Op::Sub)) {
        return false;
    }
    auto is_var = [&](ExprId id) {
        const Expr& x = ExprOf(program, id);
        return x.kind == ExprKind::VarRef && x.ref == var;
    };
    std::uint16_t c = 0;
    if (is_var(v.a) && ConstValue(program, v.b, c)) {
        step = v.op == Op::Sub ? static_cast<std::uint16_t>(-c) : c;
        return true;
    }
    if (v.op == Op::Add && is_var(v.b) && ConstValue(program, v.a, c)) {
        step = c;
        return true;
    }
    return false;
}

void FindSteps(const Program& program, ExprId id, int depth, std::vector<StepUse>& uses) {
    if (id < 0 || depth > kMaxDepth) {
        return;
    }
    const Expr& e = ExprOf(program, id);
    switch (e.kind) {
    case ExprKind::Set: {
        const Expr& place = ExprOf(program, e.a);
        if (place.kind == ExprKind::VarRef) {
            StepUse& u = uses[static_cast<std::size_t>(place.ref)];
            std::uint16_t step = 0;
            if (ConstStep(program, place.ref, e.b, step)) {
                u.steps++;
            } else {
                u.other = true;
            }
        }
        break;
    }
    case ExprKind::Let:
    case ExprKind::For:
        uses[static_cast<std::size_t>(e.ref)].other = true;
        break;
    default:
        break;
    }
    FindSteps(program, e.a, depth + 1, uses);
    FindSteps(program, e.b, depth + 1, uses);
    FindSteps(program, e.c, depth + 1, uses);
    if (e.kind == ExprKind::Do || e.kind == ExprKind::Call || e.kind == ExprKind::StructLit) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            FindSteps(program, ListItem(program, e, i), depth + 1, uses);
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            FindSteps(program, program.clauses[e.first + i].body, depth + 1, uses);
        }
    }
}

bool IsBound(const Gen& g, std::int32_t var) {
    for (const auto& b : g.bound) {
        if (b.first == var) {
            return true;
        }
    }
    return false;
}

const SteppedVar* FindStepped(const Gen& g, std::int32_t var) {
    for (const SteppedVar& s : g.stepped) {
        if (s.var == var) {
            return &s;
        }
    }
    return nullptr;
}

// a variable only named by code: a local or parameter, no address, no alias
bool Steppable(const Gen& g, std::int32_t var) {
    const Var& v = g.program->vars[static_cast<std::size_t>(var)];
    if ((v.kind != VarKind::Local && v.kind != VarKind::Param) || v.address_taken ||
        v.placement.has_address || KindOf(*g.program, v.type) != TypeKind::U16) {
        return false;
    }
    if (var == g.pin[0] || var == g.pin[1] || IsBound(g, var) || FindStepped(g, var)) {
        return false;
    }
    if (g.be->aliases[static_cast<std::size_t>(var)].value >= 0) {
        return false;
    }
    for (std::int32_t r : g.reduced) {
        if (r == var) {
            return false;
        }
    }
    return true;
}

// the index is exactly the variable's two bytes in memory
std::int32_t SteppedIndex(const Gen& g, const Opnd& index) {
    if (index.size != 2 || index.in_a || index.temp || index.index.used) {
        return -1;
    }
    const ByteRef& low = index.b[0];
    const ByteRef& high = index.b[1];
    if (low.kind != ByteKind::Mem || high.kind != ByteKind::Mem || low.value.sym < 0 ||
        high.value.sym != low.value.sym || low.value.offset != 0 || high.value.offset != 1 ||
        low.value.part != Part::Whole || high.value.part != Part::Whole) {
        return -1;
    }
    const SymInfo& info = g.be->info[static_cast<std::size_t>(low.value.sym)];
    if (info.role != SymRole::Var) {
        return -1;
    }
    return FindStepped(g, info.owner) ? info.owner : -1;
}

AsmValue StepPointer(Gen& g, const SteppedVar& s, AsmValue base, std::int32_t var,
                     std::int32_t offset) {
    for (const Derived& d : g.derived) {
        bool same = d.owner == s.loop && d.loop == s.var && d.var == var && d.offset == offset &&
                    d.base.sym == base.sym && d.base.offset == base.offset &&
                    d.base.part == base.part;
        if (same) {
            return d.ptr;
        }
    }
    Derived d;
    d.loop = s.var;
    d.owner = s.loop;
    d.base = base;
    d.var = var;
    d.offset = offset;
    d.ptr = NewLoopPointer(g, false);
    g.derived.push_back(d);
    return d.ptr;
}

// ptr = base + offset + var (only A and the carry)
void SetStepPointer(Gen& g, const Derived& d) {
    Opnd ptr = MemOpnd(d.ptr, 2, 1);
    Opnd v = MemOpnd(SymValue(g.be->var_sym[static_cast<std::size_t>(d.loop)]), 2, 1);
    Opnd base;
    if (d.var >= 0) {
        base = MemOpnd(d.base, 2, 1);
        if (d.offset != 0) {
            Combine16(g, Op::Add, base, ImmOpnd(static_cast<std::uint16_t>(d.offset & 0xFFFF), 2),
                      ptr);
            base = ptr;
        }
    } else {
        AsmValue address = d.base;
        address.offset = (address.offset + d.offset) & 0xFFFF;
        base = d.base.sym >= 0 ? SymImmOpnd(address, 2)
                               : ImmOpnd(static_cast<std::uint16_t>(address.offset), 2);
    }
    Combine16(g, Op::Add, base, v, ptr);
}

// ptr += step (only A and the carry)
void MoveStepPointer(Gen& g, const Derived& d, std::uint16_t step) {
    AsmValue low = d.ptr;
    AsmValue high = SymValue(d.ptr.sym, d.ptr.offset + 1);
    std::int32_t skip = NewLabel(g);
    if (step == 1) {
        Emit(g, Mn::INC, Mode::Abs, low);
        Branch(g, Mn::BNE, skip);
        Emit(g, Mn::INC, Mode::Abs, high);
        PlaceLabel(g, skip);
        return;
    }
    if (step == 0xFFFF) {
        Emit(g, Mn::LDA, Mode::Abs, low);
        Branch(g, Mn::BNE, skip);
        Emit(g, Mn::DEC, Mode::Abs, high);
        PlaceLabel(g, skip);
        Emit(g, Mn::DEC, Mode::Abs, low);
        return;
    }
    if (step < 0x100) {
        Emit(g, Mn::LDA, Mode::Abs, low);
        Emit(g, Mn::CLC, Mode::Imp);
        Emit(g, Mn::ADC, Mode::Imm, Num(step));
        Emit(g, Mn::STA, Mode::Abs, low);
        Branch(g, Mn::BCC, skip);
        Emit(g, Mn::INC, Mode::Abs, high);
        PlaceLabel(g, skip);
        return;
    }
    Opnd ptr = MemOpnd(d.ptr, 2, 1);
    Combine16(g, Op::Add, ptr, ImmOpnd(step, 2), ptr);
}

} // namespace

std::size_t BeginStepped(Gen& g, ExprId loop, std::int32_t counter) {
    std::size_t mark = g.stepped.size();
    const Program& program = *g.program;
    std::vector<StepUse> uses(program.vars.size());
    FindSteps(program, loop, 0, uses);
    for (std::size_t v = 0; v < uses.size(); v++) {
        std::int32_t var = static_cast<std::int32_t>(v);
        bool own = var == counter;
        bool stepped = uses[v].steps > 0 && !uses[v].other;
        if ((own || stepped) && Steppable(g, var)) {
            SteppedVar s;
            s.var = var;
            s.loop = loop;
            g.stepped.push_back(s);
        }
    }
    return mark;
}

void EndStepped(Gen& g, ExprId loop, std::size_t mark, std::size_t at) {
    auto mine = [&](std::int32_t var) {
        for (std::size_t i = mark; i < g.stepped.size(); i++) {
            if (g.stepped[i].var == var) {
                return true;
            }
        }
        return false;
    };
    std::vector<Derived> used;
    std::vector<Derived> rest;
    for (const Derived& d : g.derived) {
        if (d.owner != loop || !mine(d.loop)) {
            rest.push_back(d);
            continue;
        }
        bool referenced = false;
        for (std::size_t i = at; i < g.out->size() && !referenced; i++) {
            const AsmItem& item = (*g.out)[i];
            referenced = item.kind == ItemKind::Ins && item.value.sym == d.ptr.sym &&
                         item.value.offset == d.ptr.offset;
        }
        if (referenced) {
            used.push_back(d);
        }
    }
    g.derived = rest;
    std::vector<StepFixup> fixups;
    std::vector<StepFixup> others;
    for (const StepFixup& f : g.step_fixups) {
        (mine(f.var) ? fixups : others).push_back(f);
    }
    g.step_fixups = others;
    g.stepped.resize(mark);
    if (used.empty()) {
        return;
    }
    // runs inside the loop, at the steps
    g.loop_depth++;
    for (const StepFixup& f : fixups) {
        std::size_t place = g.out->size();
        for (std::size_t i = at; i < g.out->size(); i++) {
            const AsmItem& item = (*g.out)[i];
            if (item.kind == ItemKind::Label && item.value.sym == f.label) {
                place = i;
                break;
            }
        }
        if (place == g.out->size()) {
            continue;
        }
        std::size_t before = g.out->size();
        for (const Derived& d : used) {
            if (d.loop == f.var && f.reset) {
                SetStepPointer(g, d);
            } else if (d.loop == f.var) {
                MoveStepPointer(g, d, f.step);
            }
        }
        std::rotate(g.out->begin() + static_cast<std::ptrdiff_t>(place),
                    g.out->begin() + static_cast<std::ptrdiff_t>(before), g.out->end());
    }
    g.loop_depth--;
    // runs once, before the loop
    std::size_t before = g.out->size();
    for (const Derived& d : used) {
        SetStepPointer(g, d);
    }
    std::rotate(g.out->begin() + static_cast<std::ptrdiff_t>(at),
                g.out->begin() + static_cast<std::ptrdiff_t>(before), g.out->end());
}

bool UseStepPointer(Gen& g, PlaceRef& place) {
    if (!place.has_index || place.mult != 1 || place.delta != 0) {
        return false;
    }
    std::int32_t v = SteppedIndex(g, place.index);
    if (v < 0) {
        return false;
    }
    const SteppedVar& s = *FindStepped(g, v);
    AsmValue base;
    std::int32_t var = -1;
    if (!place.pointer) {
        base = place.base;
    } else if (place.ptr_const) {
        base = Num(place.ptr_value);
    } else if (place.ptr_var >= 0 && !MayWriteVarIn(g, s.loop, place.ptr_var)) {
        var = place.ptr_var;
        base = SymValue(g.be->var_sym[static_cast<std::size_t>(var)]);
    } else {
        return false;
    }
    place.ptr = StepPointer(g, s, base, var, static_cast<std::int32_t>(place.cursor.address));
    place.ptr2 = AsmValue();
    place.pointer = true;
    place.step_ptr = true;
    place.cursor.address = 0;
    place.has_index = false;
    place.index = Opnd();
    place.mult = 0;
    place.counter = -1;
    return true;
}

bool IsSteppedIndex(const Gen& g, const Opnd& index) {
    return SteppedIndex(g, index) >= 0;
}

void StepOwnPointers(Gen& g, ExprId loop, std::int32_t var, std::uint16_t step) {
    std::vector<Derived> own;
    for (const Derived& d : g.derived) {
        if (d.owner == loop && d.loop == var) {
            own.push_back(d);
        }
    }
    for (const Derived& d : own) {
        MoveStepPointer(g, d, step);
    }
}

void NoteSet(Gen& g, ExprId place, ExprId value) {
    const Expr& p = At(g, place);
    if (p.kind != ExprKind::VarRef || !FindStepped(g, p.ref)) {
        return;
    }
    // the loop sets it only this way (FindSteps)
    std::uint16_t step = 0;
    if (ConstStep(*g.program, p.ref, value, step)) {
        NoteStep(g, p.ref, step);
        return;
    }
    Fail(g, At(g, place).loc, "internal: a stepped variable set another way");
}

void NoteReset(Gen& g, std::int32_t var) {
    if (!FindStepped(g, var)) {
        return;
    }
    StepFixup f;
    f.label = NewLabel(g);
    f.var = var;
    f.reset = true;
    g.step_fixups.push_back(f);
    PlaceLabel(g, f.label);
}

void NoteStep(Gen& g, std::int32_t var, std::uint16_t step) {
    if (!FindStepped(g, var) || step == 0) {
        return;
    }
    StepFixup f;
    f.label = NewLabel(g);
    f.var = var;
    f.step = step;
    g.step_fixups.push_back(f);
    PlaceLabel(g, f.label);
}

} // namespace tisple
