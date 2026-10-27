#include "backend/gen.hpp"

#include <algorithm>

// While loops, with variables in registers when the loop allows. A byte variable (u8 or i8)
// the loop reads may live in X or Y for the whole loop: loaded before it, its sets write the
// register (INX, DEY, or TAX after the value), and when the loop sets it the register is
// stored back where the loop ends. A u16 variable may live in Y and memory like a u16 for
// counter (gen_loop.cpp): low byte in Y, high byte in its own memory, and every array or
// pointer it indexes read through a derived pointer, base + (high << 8), set before the loop.
// Each of its sets moves the pointers' high bytes with its own; the code for that is put in
// once the body is done and the pointers are known (WideFixup).
//
// As for u8 for loops, the body is generated once per choice, a choice whose code touches
// its registers (or calls anything) is dropped, and the cheapest is kept, cycles inside loops
// weighing more. Facts the condition gives hold in the body (gen_facts.cpp).
//
// A register an outer u8 for loop keeps its counter in may be taken too: the counter is
// stored to its variable before the loop (it never changes inside, so reads read that) and
// loaded back where the loop ends. A loop with a return in it does not, since the return
// would leave without the counter.

namespace tisple {

namespace {

// a while loop tries registers only this deep among other loops that do
constexpr int kMaxPinnedDepth = 3;
// a test this small is copied before the loop (expressions in it)
constexpr std::uint32_t kMaxRotatedTest = 24;
constexpr int kMaxDepth = 64;

const Expr& At(const Gen& g, ExprId id) {
    return ExprOf(*g.program, id);
}

struct VarUse {
    int uses = 0;
    bool written = false;
    bool declared = false; // a let or for inside the loop
};

void CountUses(const Program& program, ExprId id, int depth, std::vector<VarUse>& uses) {
    if (id < 0 || depth > kMaxDepth) {
        return;
    }
    const Expr& e = ExprOf(program, id);
    switch (e.kind) {
    case ExprKind::VarRef:
        uses[static_cast<std::size_t>(e.ref)].uses++;
        break;
    case ExprKind::Set: {
        const Expr& place = ExprOf(program, e.a);
        if (place.kind == ExprKind::VarRef) {
            uses[static_cast<std::size_t>(place.ref)].written = true;
        }
        break;
    }
    case ExprKind::Let:
    case ExprKind::For:
        uses[static_cast<std::size_t>(e.ref)].declared = true;
        break;
    default:
        break;
    }
    CountUses(program, e.a, depth + 1, uses);
    CountUses(program, e.b, depth + 1, uses);
    CountUses(program, e.c, depth + 1, uses);
    if (e.kind == ExprKind::Do || e.kind == ExprKind::Call || e.kind == ExprKind::StructLit) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            CountUses(program, ListItem(program, e, i), depth + 1, uses);
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            CountUses(program, program.clauses[e.first + i].body, depth + 1, uses);
        }
    }
}

// some item from `from` on jumps or branches to the label
bool Names(const Gen& g, std::size_t from, std::int32_t label) {
    for (std::size_t i = from; i < g.out->size(); i++) {
        const AsmItem& item = (*g.out)[i];
        if (item.kind != ItemKind::Label && item.value.sym == label) {
            return true;
        }
    }
    return false;
}

bool IsBound(const Gen& g, std::int32_t var) {
    for (const auto& b : g.bound) {
        if (b.first == var) {
            return true;
        }
    }
    return false;
}

// a variable a register can stand for: a local or parameter only code reads by name
bool Pinnable(const Gen& g, std::int32_t var, const VarUse& use) {
    const Var& v = g.program->vars[static_cast<std::size_t>(var)];
    if (use.uses == 0 || use.declared) {
        return false;
    }
    if ((v.kind != VarKind::Local && v.kind != VarKind::Param) || v.address_taken || v.loop_var ||
        v.placement.has_address) {
        return false;
    }
    if (var == g.pin[0] || var == g.pin[1] || IsBound(g, var)) {
        return false;
    }
    if (g.be->aliases[static_cast<std::size_t>(var)].value >= 0) {
        return false; // its reads read the let's value instead
    }
    for (std::int32_t r : g.reduced) {
        if (r == var) {
            return false;
        }
    }
    return true;
}

// the registers a loop tries: X and Y for byte variables, or Y for a u16 one (wide); spill:
// the register held an outer loop's counter, kept in memory meanwhile
struct WhileRegs {
    std::int32_t var[2] = {-1, -1};
    bool wide = false;
    bool written[2] = {false, false};
    bool spill[2] = {false, false};
};

bool HasReturn(const Program& program, ExprId id, int depth) {
    if (id < 0 || depth > kMaxDepth) {
        return false;
    }
    const Expr& e = ExprOf(program, id);
    if (e.kind == ExprKind::Return) {
        return true;
    }
    if (HasReturn(program, e.a, depth + 1) || HasReturn(program, e.b, depth + 1) ||
        HasReturn(program, e.c, depth + 1)) {
        return true;
    }
    if (e.kind == ExprKind::Do || e.kind == ExprKind::Call || e.kind == ExprKind::StructLit) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (HasReturn(program, ListItem(program, e, i), depth + 1)) {
                return true;
            }
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (HasReturn(program, program.clauses[e.first + i].body, depth + 1)) {
                return true;
            }
        }
    }
    return false;
}

// register r holds a u8 for counter this loop may keep in memory instead
bool Spillable(const Gen& g, int r, ExprId id) {
    std::int32_t counter = g.pin[r];
    if (counter < 0 || g.wide) {
        return false;
    }
    const Var& v = g.program->vars[static_cast<std::size_t>(counter)];
    TypeKind kind = KindOf(*g.program, v.type);
    return v.loop_var && (kind == TypeKind::U8 || kind == TypeKind::I8) &&
           !HasReturn(*g.program, id, 0);
}

std::vector<WhileRegs> Choices(const Gen& g, ExprId id) {
    std::vector<WhileRegs> out;
    out.push_back(WhileRegs());
    if (g.for_depth + g.while_pins >= kMaxPinnedDepth) {
        return out;
    }
    const Program& program = *g.program;
    std::vector<VarUse> uses(program.vars.size());
    CountUses(program, id, 0, uses);
    std::vector<std::int32_t> bytes;
    std::vector<std::int32_t> words;
    for (std::size_t v = 0; v < uses.size(); v++) {
        std::int32_t var = static_cast<std::int32_t>(v);
        if (!Pinnable(g, var, uses[v])) {
            continue;
        }
        TypeKind kind = KindOf(program, program.vars[v].type);
        if (kind == TypeKind::U8 || kind == TypeKind::I8) {
            bytes.push_back(var);
        } else if (kind == TypeKind::U16) {
            words.push_back(var);
        }
    }
    auto by_uses = [&](std::int32_t a, std::int32_t b) {
        return uses[static_cast<std::size_t>(a)].uses > uses[static_cast<std::size_t>(b)].uses;
    };
    std::stable_sort(bytes.begin(), bytes.end(), by_uses);
    std::stable_sort(words.begin(), words.end(), by_uses);
    bool free[2] = {g.pin[0] < 0, g.pin[1] < 0 && !g.wide};
    bool spill[2] = {Spillable(g, 0, id), Spillable(g, 1, id)};
    auto add = [&](std::int32_t x, std::int32_t y, bool wide) {
        WhileRegs r;
        r.var[0] = x;
        r.var[1] = y;
        r.wide = wide;
        for (int k = 0; k < 2; k++) {
            if (r.var[k] < 0) {
                continue;
            }
            if (!free[k] && !spill[k]) {
                return;
            }
            r.spill[k] = !free[k];
            r.written[k] = uses[static_cast<std::size_t>(r.var[k])].written;
        }
        out.push_back(r);
    };
    if (!bytes.empty()) {
        add(bytes[0], -1, false);
        add(-1, bytes[0], false);
    }
    if (bytes.size() >= 2) {
        add(bytes[0], bytes[1], false);
        add(bytes[1], bytes[0], false);
    }
    if (!words.empty()) {
        add(-1, words[0], true);
        if (!bytes.empty()) {
            add(bytes[0], words[0], true);
        }
    }
    return out;
}

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
    if (is_var(v.a) && ConstValue(*g.program, v.b, c)) {
        c = v.op == Op::Sub ? static_cast<std::uint16_t>(-c) : c;
        return true;
    }
    return v.op == Op::Add && is_var(v.b) && ConstValue(*g.program, v.a, c);
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

// the loop with the registers of `regs`; false (with nothing kept) if its code needed them
bool LoopWith(Gen& g, ExprId id, const WhileRegs& regs) {
    const Expr& e = At(g, id);
    std::uint16_t c = 0;
    bool fixed = ConstValue(*g.program, e.a, c);
    bool pinned = regs.var[0] >= 0 || regs.var[1] >= 0;
    bool outer = g.violation;
    if (pinned) {
        g.violation = false;
    }
    AsmValue sym[2];
    std::size_t loads_at = g.out->size();
    // an outer loop's counter goes to its variable while the loop has its register
    std::int32_t spilled[2] = {-1, -1};
    ExprId spilled_body[2] = {-1, -1};
    for (int r = 0; r < 2; r++) {
        if (!regs.spill[r]) {
            continue;
        }
        spilled[r] = g.pin[r];
        spilled_body[r] = g.pin_body[r];
        AsmValue home = SymValue(g.be->var_sym[static_cast<std::size_t>(spilled[r])]);
        Emit(g, r == 0 ? Mn::STX : Mn::STY, Mode::Abs, home);
        g.pin[r] = -1;
        g.pin_body[r] = -1;
    }
    for (int r = 0; r < 2; r++) {
        if (regs.var[r] < 0) {
            continue;
        }
        sym[r] = SymValue(g.be->var_sym[static_cast<std::size_t>(regs.var[r])]);
        Emit(g, r == 0 ? Mn::LDX : Mn::LDY, Mode::Abs, sym[r]);
    }
    std::size_t init_at = g.out->size();
    WideLoop wide;
    if (regs.wide) {
        wide.var = regs.var[1];
        wide.high = SymValue(sym[1].sym, 1);
        wide.body = id;
        g.wide = &wide;
        g.wide_fixups.clear();
    }
    for (int r = 0; r < 2; r++) {
        if (regs.var[r] >= 0) {
            g.pin[r] = regs.var[r];
            g.pin_body[r] = id;
        }
    }
    if (pinned) {
        g.while_pins++;
    }
    // the u16 variables the loop only steps, but not the ones it keeps in registers, index
    // through step pointers (gen_step.cpp)
    std::size_t stepped = BeginStepped(g, id, -1);
    bool top_test = !fixed && regs.wide && EndsWithUnitStep(g, e.b, regs.var[1]);
    // the test also before the loop instead of a jump to the one at the bottom, when it is
    // small: then the bottom test follows the body straight on, flags and all
    bool rotate = !fixed && !top_test && CountExprs(*g.program, e.a) <= kMaxRotatedTest &&
                  !HasCalls(*g.program, e.a);
    std::int32_t top = NewLabel(g);
    std::int32_t test = NewLabel(g);
    std::int32_t end = NewLabel(g);
    LoopCtx ctx;
    ctx.break_label = end;
    ctx.continue_label = fixed || top_test ? top : test;
    g.loops.push_back(ctx);
    g.loop_factors.resize(static_cast<std::size_t>(g.loop_depth));
    g.loop_factors.push_back(LoopFactor(g, id));
    g.loop_depth++;
    if (top_test) {
        PlaceLabel(g, top);
        GenBranch(g, e.a, false, end);
    } else if (rotate) {
        // runs once each time the loop starts
        g.loop_depth--;
        GenBranch(g, e.a, false, end);
        g.loop_depth++;
        PlaceLabel(g, top);
    } else if (!fixed) {
        Jump(g, test);
        PlaceLabel(g, top);
    } else {
        PlaceLabel(g, top);
    }
    std::size_t body_at = g.out->size();
    std::size_t facts = PushFacts(g, e.a, true, -1);
    GenStmt(g, e.b);
    PopFacts(g, facts);
    if (fixed || top_test) {
        Jump(g, top);
    } else {
        // with the test rotated, the label is only for a continue
        if (!rotate || Names(g, body_at, test)) {
            PlaceLabel(g, test);
        }
        GenBranch(g, e.a, true, top);
    }
    g.loops.pop_back();
    PlaceLabel(g, end);
    for (int r = 0; r < 2; r++) {
        if (regs.var[r] >= 0) {
            g.pin[r] = -1;
            g.pin_body[r] = -1;
        }
    }
    g.wide = regs.wide ? nullptr : g.wide;
    if (pinned) {
        g.while_pins--;
    }
    if (pinned && g.violation && !g.failed) {
        g.loop_depth--;
        return false;
    }
    if (regs.var[1] >= 0) {
        std::vector<Derived> used = TakeDerived(g, regs.var[1], init_at);
        AsmValue high = regs.wide ? wide.high : AsmValue();
        if (regs.wide) {
            ApplyFixups(g, used, high, init_at);
        }
        InitDerived(g, used, init_at, high);
    }
    g.loop_depth--;
    EndStepped(g, id, stepped, loads_at);
    for (int r = 0; r < 2; r++) {
        if (regs.var[r] >= 0 && regs.written[r]) {
            Emit(g, r == 0 ? Mn::STX : Mn::STY, Mode::Abs, sym[r]);
        }
    }
    for (int r = 0; r < 2; r++) {
        if (spilled[r] < 0) {
            continue;
        }
        AsmValue home = SymValue(g.be->var_sym[static_cast<std::size_t>(spilled[r])]);
        Emit(g, r == 0 ? Mn::LDX : Mn::LDY, Mode::Abs, home);
        g.pin[r] = spilled[r];
        g.pin_body[r] = spilled_body[r];
    }
    if (pinned) {
        g.violation = outer;
    }
    return true;
}

} // namespace

void EmitPinned(Gen& g, Mn mn, Mode mode, AsmValue value) {
    std::int32_t pins[2] = {g.pin[0], g.pin[1]};
    g.pin[0] = -1;
    g.pin[1] = -1;
    Emit(g, mn, mode, value);
    g.pin[0] = pins[0];
    g.pin[1] = pins[1];
}

bool SetPinnedVar(Gen& g, ExprId place, ExprId value) {
    const Expr& p = At(g, place);
    if (p.kind != ExprKind::VarRef || g.program->vars[static_cast<std::size_t>(p.ref)].loop_var) {
        return false;
    }
    int r = g.pin[0] == p.ref ? 0 : g.pin[1] == p.ref ? 1 : -1;
    if (r < 0) {
        return false;
    }
    if (g.wide && g.wide->var == p.ref) {
        SetWide(g, value);
        return true;
    }
    std::uint16_t c = 0;
    if (StepOf(g, p.ref, value, c) && ((c & 0xFF) == 1 || (c & 0xFF) == 0xFF)) {
        bool up = (c & 0xFF) == 1;
        EmitPinned(g, r == 0 ? (up ? Mn::INX : Mn::DEX) : (up ? Mn::INY : Mn::DEY));
        return true;
    }
    if (StepOf(g, p.ref, value, c) && (c & 0xFF) == 0) {
        return true;
    }
    EvalA(g, value);
    EmitPinned(g, r == 0 ? Mn::TAX : Mn::TAY);
    return true;
}

void GenWhile(Gen& g, ExprId id) {
    const Expr& e = At(g, id);
    std::uint16_t c = 0;
    if (ConstValue(*g.program, e.a, c) && c == 0) {
        return;
    }
    std::vector<WhileRegs> choices = Choices(g, id);
    std::size_t mark = g.out->size();
    Gen start = g;
    bool found = false;
    std::uint64_t best_cost = 0;
    std::vector<AsmItem> best_items;
    Gen best = g;
    for (const WhileRegs& regs : choices) {
        g = start;
        g.out->resize(mark);
        if (!LoopWith(g, id, regs)) {
            continue;
        }
        std::uint64_t cost = g.cost - start.cost;
        if (!found || cost < best_cost) {
            found = true;
            best_cost = cost;
            best_items.assign(g.out->begin() + static_cast<std::ptrdiff_t>(mark), g.out->end());
            best = g;
        }
        if (g.failed) {
            break;
        }
    }
    g = best;
    g.out->resize(mark);
    g.out->insert(g.out->end(), best_items.begin(), best_items.end());
}

} // namespace tisple
