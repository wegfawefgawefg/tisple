#include "backend/gen.hpp"

#include <algorithm>

// While loops, with variables in registers when the loop allows. A byte variable (u8 or i8)
// the loop reads may live in X or Y for the whole loop: loaded before it, its sets write the
// register (INX, DEY, or TAX after the value), and when the loop sets it the register is
// stored back where the loop ends. A u16 variable may live in Y and memory
// (gen_while_wide.cpp).
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
    ExprId bump = -1; // the compare done against its variable plus one
};

// A compare of the test, e > v or v < e for a u8 variable v the loop never sets, is
// e >= v + 1, with v + 1 worked out once before the loop: then the compare loads e into A,
// where it often is what the body wants first (insertion sort moves the byte it compared),
// and v needs no register. When v is 255 the compare never holds, and since it is one of the
// test's and-ed parts, the loop does not run. Returns the compare and v, or -1.
ExprId FindBump(const Gen& g, ExprId id, const std::vector<VarUse>& uses, std::int32_t& var) {
    const Program& program = *g.program;
    id = ForwardTarget(g, id);
    const Expr& e = At(g, id);
    if (e.kind == ExprKind::And) {
        ExprId found = FindBump(g, e.a, uses, var);
        return found >= 0 ? found : FindBump(g, e.b, uses, var);
    }
    // (only the strict ones: e <= 255 always holds, and the loop would not run)
    if (e.kind != ExprKind::Binary || (e.op != Op::Gt && e.op != Op::Lt) ||
        ExprKindOf(program, e.a) != TypeKind::U8) {
        return -1;
    }
    bool left = e.op == Op::Gt;
    const Expr& side = At(g, left ? e.b : e.a);
    std::uint16_t c = 0;
    if (side.kind != ExprKind::VarRef || ConstValue(program, left ? e.a : e.b, c)) {
        return -1;
    }
    const Var& v = program.vars[static_cast<std::size_t>(side.ref)];
    const VarUse& use = uses[static_cast<std::size_t>(side.ref)];
    if ((v.kind != VarKind::Local && v.kind != VarKind::Param) || v.address_taken ||
        v.placement.has_address || use.written || use.declared ||
        KindOf(program, v.type) != TypeKind::U8) {
        return -1;
    }
    var = side.ref;
    return id;
}

// the first instruction at or after i, past notes and labels
std::size_t NextIns(const Gen& g, std::size_t i) {
    while (i < g.out->size() && (*g.out)[i].kind != ItemKind::Ins) {
        i++;
    }
    return i;
}

bool SameIns(const AsmItem& a, const AsmItem& b) {
    return a.kind == ItemKind::Ins && b.kind == ItemKind::Ins && a.mn == b.mn && a.mode == b.mode &&
           a.value.sym == b.value.sym && a.value.offset == b.value.offset &&
           a.value.part == b.value.part;
}

// a rotated loop whose tests both end LDA x / CMP y / branch, and whose body starts LDA x: the
// peephole drops that load, since both ways in hold x (its cycles, at the loop's weight)
std::uint64_t TopLoadSaved(const Gen& g, std::size_t top_at, std::size_t bottom_at) {
    std::size_t first = NextIns(g, top_at);
    if (first >= g.out->size() || (*g.out)[first].mn != Mn::LDA) {
        return 0;
    }
    auto ends_with_load = [&](std::size_t branch) {
        std::size_t cmp = branch;
        std::size_t load = branch;
        int seen = 0;
        for (std::size_t i = branch; i-- > 0 && seen < 2;) {
            if ((*g.out)[i].kind == ItemKind::Ins) {
                (seen == 0 ? cmp : load) = i;
                seen++;
            } else if ((*g.out)[i].kind != ItemKind::Note) {
                return false;
            }
        }
        return seen == 2 && (*g.out)[cmp].mn == Mn::CMP && SameIns((*g.out)[load], (*g.out)[first]);
    };
    // the entry test's branch is the last instruction before top, the bottom one the last
    std::size_t entry = top_at;
    while (entry > 0 && (*g.out)[entry - 1].kind != ItemKind::Ins) {
        entry--;
    }
    std::size_t bottom = g.out->size();
    while (bottom > bottom_at && (*g.out)[bottom - 1].kind != ItemKind::Ins) {
        bottom--;
    }
    if (entry == 0 || bottom <= bottom_at || !IsBranch((*g.out)[entry - 1].mn) ||
        !IsBranch((*g.out)[bottom - 1].mn) || !ends_with_load(entry - 1) ||
        !ends_with_load(bottom - 1)) {
        return 0;
    }
    return static_cast<std::uint64_t>(ItemCycles(g.be->as, (*g.out)[first])) * LoopWeight(g);
}

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
    std::int32_t bump_var = -1;
    const Expr& e = At(g, id);
    ExprId bump =
        HasEffects(program, e.a) || HasCalls(program, id) ? -1 : FindBump(g, e.a, uses, bump_var);
    if (bump >= 0) {
        // the same choices, the variable compared out of the registers
        std::vector<std::int32_t> others;
        for (std::int32_t b : bytes) {
            if (b != bump_var) {
                others.push_back(b);
            }
        }
        WhileRegs plain;
        plain.bump = bump;
        out.push_back(plain);
        std::size_t before = out.size();
        if (!others.empty()) {
            add(others[0], -1, false);
            add(-1, others[0], false);
        }
        if (others.size() >= 2) {
            add(others[0], others[1], false);
            add(others[1], others[0], false);
        }
        for (std::size_t i = before; i < out.size(); i++) {
            out[i].bump = bump;
        }
    }
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
    if (regs.bump >= 0) {
        // v + 1, once each time the loop starts; v = 255 never compares true
        const Expr& cmp = At(g, regs.bump);
        bool left = cmp.op == Op::Gt;
        g.loop_depth--;
        EvalA(g, left ? cmp.b : cmp.a);
        Opnd t = NewTemp(g, 1);
        Emit(g, Mn::CLC, Mode::Imp);
        Emit(g, Mn::ADC, Mode::Imm, Num(1));
        Emit(g, Mn::STA, Mode::Abs, t.b[0].value);
        Branch(g, Mn::BCS, end);
        g.loop_depth++;
        g.bumped.push_back({regs.bump, t});
    }
    std::size_t top_at = 0;
    if (top_test) {
        PlaceLabel(g, top);
        GenBranch(g, e.a, false, end);
    } else if (rotate) {
        // runs once each time the loop starts
        g.loop_depth--;
        GenBranch(g, e.a, false, end);
        g.loop_depth++;
        top_at = g.out->size();
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
        std::size_t bottom_at = g.out->size();
        GenBranch(g, e.a, true, top);
        if (rotate) {
            g.cost -= std::min(g.cost, TopLoadSaved(g, top_at, bottom_at));
        }
    }
    if (regs.bump >= 0) {
        g.bumped.pop_back();
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
    const Derived* rebuild = nullptr;
    std::vector<Derived> used;
    if (regs.var[1] >= 0) {
        used = TakeDerived(g, regs.var[1], init_at);
        AsmValue high = regs.wide ? wide.high : AsmValue();
        if (regs.wide) {
            ApplyFixups(g, used, high, init_at);
            rebuild = DropHighSteps(g, used, high, init_at);
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
    if (rebuild) {
        // the high byte from the pointer that moved with it
        AsmValue base = rebuild->base;
        base.offset = (base.offset + rebuild->offset) & 0xFFFF;
        Opnd start = base.sym >= 0 ? SymImmOpnd(base, 2)
                                   : ImmOpnd(static_cast<std::uint16_t>(base.offset), 2);
        Emit(g, Mn::LDA, Mode::Abs, SymValue(rebuild->ptr.sym, rebuild->ptr.offset + 1));
        Emit(g, Mn::SEC, Mode::Imp);
        OpA(g, Mn::SBC, start, 1);
        Emit(g, Mn::STA, Mode::Abs, wide.high);
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
