#include "backend/gen.hpp"
#include "check/arith.hpp"

#include <algorithm>

namespace tisple {

namespace {

// a u8 counter nests this deep at most in registers (each level tries X, Y, memory)
constexpr int kMaxPinnedDepth = 3;
// a loop whose counter nothing reads runs as copies of its body when it is this small:
// trips, and expressions in all the copies together
constexpr std::int32_t kMaxUnrollTrips = 16;
constexpr std::uint32_t kMaxUnrollExprs = 160;

struct ForParts {
    ExprId id = -1;
    std::int32_t var = -1;
    std::uint32_t size = 1;
    Opnd start;
    Opnd end; // raw bits the counter stops at: Imm or plain memory
    ExprId body = -1;
    std::int32_t end_label = -1;
    // the body never reads the counter and the trip count is a constant: count it down to 0
    // instead (DEX / BNE), from trips (0 for 256)
    bool down = false;
    std::uint8_t trips = 0;
};

bool IsPlainMem(const Opnd& o) {
    for (int k = 0; k < o.size; k++) {
        if (o.b[k].kind != ByteKind::Mem) {
            return false;
        }
    }
    return o.size != 0 && !o.in_a;
}

// the end value stays as it is only if the body cannot change what it reads
bool EndIsStable(const Gen& g, const Opnd& end, ExprId body) {
    if (IsImm(end) || end.temp) {
        return true;
    }
    if (!IsPlainMem(end)) {
        return false;
    }
    std::int32_t sym = end.b[0].value.sym;
    if (sym < 0) {
        return false;
    }
    const SymInfo& info = g.be->info[static_cast<std::size_t>(sym)];
    if (info.role != SymRole::Var) {
        return false;
    }
    return !MayWriteVarIn(g, body, info.owner);
}

void LoadCounter(Gen& g, int r, const Opnd& start) {
    const ByteRef& b = start.b[0];
    if (!start.in_a && (b.kind == ByteKind::Imm || b.kind == ByteKind::Mem)) {
        Emit(g, r == 0 ? Mn::LDX : Mn::LDY, b.kind == ByteKind::Imm ? Mode::Imm : Mode::Abs,
             b.value);
        return;
    }
    if (!start.in_a && b.kind == ByteKind::Reg) {
        AsmValue scratch = SymValue(g.be->scratch);
        Emit(g, b.reg == 0 ? Mn::STX : Mn::STY, Mode::Abs, scratch);
        Emit(g, r == 0 ? Mn::LDX : Mn::LDY, Mode::Abs, scratch);
        return;
    }
    LoadA(g, start, 0);
    Emit(g, r == 0 ? Mn::TAX : Mn::TAY, Mode::Imp);
}

bool IsImmZeroByte(const Opnd& o, int k) {
    const ByteRef& b = o.b[k];
    return b.kind == ByteKind::Imm && b.value.sym < 0 && (b.value.offset & 0xFF) == 0;
}

// the branch just emitted goes back to the top of a u8 loop after comparing the counter,
// one past its value in the body, with the end: the counter only ever counts up to the end
// (the guard or the constant bounds saw start < end, and the body cannot write it), so going
// back means counter < end, unsigned, and the carry is clear
void MarkBackEdge(Gen& g, const ForParts& f) {
    if (KindOf(*g.program, g.program->vars[static_cast<std::size_t>(f.var)].type) == TypeKind::U8) {
        g.out->back().carry = Carry::Clear;
    }
}

// the counter in register r; false (and nothing kept) if the body needed r
bool TryPinned(Gen& g, const ForParts& f, int r) {
    std::size_t mark = g.out->size();
    Gen saved = g;
    bool outer = g.violation;
    g.violation = false;
    LoadCounter(g, r, f.down ? ImmOpnd(f.trips, 1) : f.start);
    g.pin[r] = f.var;
    g.pin_body[r] = f.body;
    std::vector<ReducedLet> reduced = FindReducedLets(g, f.var, f.body);
    InitReducedLets(g, reduced);
    std::vector<ReducedByte> bytes = FindReducedBytes(g, f.var, f.body, reduced);
    InitReducedBytes(g, bytes);
    std::size_t init_at = g.out->size();
    std::int32_t top = NewLabel(g);
    std::int32_t next = NewLabel(g);
    g.loops.back().continue_label = next;
    PlaceLabel(g, top);
    GenStmt(g, f.body);
    PlaceLabel(g, next);
    StepReducedLets(g, reduced);
    DropReducedLets(g, reduced);
    StepReducedBytes(g, bytes);
    DropReducedBytes(g, bytes);
    g.pin[r] = -1;
    g.pin_body[r] = -1;
    if (g.violation && !g.failed) {
        g.out->resize(mark);
        g = saved;
        return false;
    }
    if (f.down) {
        Emit(g, r == 0 ? Mn::DEX : Mn::DEY, Mode::Imp);
        Branch(g, Mn::BNE, top);
    } else if (!IsImmZeroByte(f.end, 0)) {
        Emit(g, r == 0 ? Mn::INX : Mn::INY, Mode::Imp);
        const ByteRef& b = f.end.b[0];
        Emit(g, r == 0 ? Mn::CPX : Mn::CPY, b.kind == ByteKind::Imm ? Mode::Imm : Mode::Abs,
             b.value);
        Branch(g, Mn::BNE, top);
        MarkBackEdge(g, f);
    } else {
        Emit(g, r == 0 ? Mn::INX : Mn::INY, Mode::Imp);
        Branch(g, Mn::BNE, top);
    }
    if (r == 1) {
        InitDerived(g, TakeDerived(g, f.var, init_at), init_at, AsmValue());
    }
    g.violation = outer;
    return true;
}

void MemoryLoop(Gen& g, const ForParts& f);

// the body once per trip, one after the other: no counter and no test; a continue goes to
// the next copy
void UnrolledLoop(Gen& g, const ForParts& f) {
    std::uint32_t trips = f.trips == 0 ? 256 : f.trips;
    for (std::uint32_t i = 0; i < trips; i++) {
        std::int32_t next = NewLabel(g);
        g.loops.back().continue_label = next;
        GenStmt(g, f.body);
        PlaceLabel(g, next);
    }
}

// a u8 counter runs from the start up to one below the end; i8 counters get no range
CounterRange CounterRangeOf(const Program& program, const Expr& e, TypeKind kind) {
    CounterRange r;
    r.var = e.ref;
    if (kind != TypeKind::U8) {
        r.var = -1;
        return r;
    }
    std::uint16_t c = 0;
    if (ConstValue(program, e.a, c) && c <= 0xFF) {
        r.lo = c;
    }
    if (e.b >= 0 && ConstValue(program, e.b, c) && c >= 1 && c <= 0x100) {
        r.hi = c - 1;
    }
    return r;
}

// the counter in X, in Y, or in memory, whichever costs the fewest cycles (inner loops
// weigh more, see Gen::cost). In memory it leaves both registers to the body: its own loops,
// or an operation on two table reads (IndexedSecond).
void ByteLoop(Gen& g, const ForParts& f, bool pin) {
    std::size_t mark = g.out->size();
    Gen start = g;
    bool found = false;
    std::uint64_t best_cost = 0;
    std::vector<AsmItem> best_items;
    Gen best = g;
    for (int choice = 0; choice < 3; choice++) {
        bool memory = choice == 2;
        if (!memory && (!pin || start.pin[choice] >= 0)) {
            continue;
        }
        g = start;
        g.out->resize(mark);
        // the body's stepped u16 variables, with this choice's code (gen_step.cpp)
        std::size_t stepped = BeginStepped(g, f.id, -1);
        if (memory) {
            MemoryLoop(g, f);
        } else if (!TryPinned(g, f, choice)) {
            continue;
        }
        EndStepped(g, f.id, stepped, mark);
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

bool IsImmByteValue(const Opnd& o, int k, int value) {
    const ByteRef& b = o.b[k];
    return b.kind == ByteKind::Imm && b.value.sym < 0 && (b.value.offset & 0xFF) == value;
}

// a u16 counter: low byte in Y, high byte in the variable's memory, arrays and pointers it
// indexes as derived (ptr),Y. False (nothing kept) if the body needed Y.
bool TryWide(Gen& g, const ForParts& f) {
    if (g.wide || g.pin[1] >= 0) {
        return false;
    }
    std::size_t mark = g.out->size();
    Gen saved = g;
    bool outer = g.violation;
    g.violation = false;
    AsmValue high = SymValue(g.be->var_sym[static_cast<std::size_t>(f.var)], 1);
    LoadA(g, f.start, 1);
    Emit(g, Mn::STA, Mode::Abs, high);
    LoadCounter(g, 1, f.start);
    std::size_t init_at = g.out->size();
    WideLoop wide;
    wide.var = f.var;
    wide.high = high;
    wide.body = f.body;
    g.wide = &wide;
    g.pin[1] = f.var;
    g.pin_body[1] = f.body;
    std::vector<ReducedLet> reduced = FindReducedLets(g, f.var, f.body);
    InitReducedLets(g, reduced);
    std::int32_t top = NewLabel(g);
    std::int32_t next = NewLabel(g);
    g.loops.back().continue_label = next;
    PlaceLabel(g, top);
    GenStmt(g, f.body);
    PlaceLabel(g, next);
    StepReducedLets(g, reduced);
    DropReducedLets(g, reduced);
    g.pin[1] = -1;
    g.pin_body[1] = -1;
    g.wide = nullptr;
    if (g.violation && !g.failed) {
        g.out->resize(mark);
        g = saved;
        return false;
    }
    std::vector<Derived> used = TakeDerived(g, f.var, init_at);
    // INY; a new page moves the high byte and every derived pointer
    Emit(g, Mn::INY, Mode::Imp);
    auto next_page = [&]() {
        for (const Derived& d : used) {
            Emit(g, Mn::INC, Mode::Abs, SymValue(d.ptr.sym, d.ptr.offset + 1));
        }
        Emit(g, Mn::INC, Mode::Abs, high);
    };
    bool end_low_zero = IsImmByteValue(f.end, 0, 0);
    if (end_low_zero && IsImmByteValue(f.end, 1, 0)) {
        Branch(g, Mn::BNE, top);
        next_page();
        Branch(g, Mn::BNE, top);
    } else if (end_low_zero) {
        // the counter can only reach the end at a page start
        Branch(g, Mn::BNE, top);
        next_page();
        Emit(g, Mn::LDA, Mode::Abs, high);
        OpA(g, Mn::CMP, f.end, 1);
        Branch(g, Mn::BNE, top);
    } else {
        std::int32_t wrap = NewLabel(g);
        std::int32_t check = NewLabel(g);
        Branch(g, Mn::BEQ, wrap);
        PlaceLabel(g, check);
        const ByteRef& low = f.end.b[0];
        Emit(g, Mn::CPY, low.kind == ByteKind::Imm ? Mode::Imm : Mode::Abs, low.value);
        Branch(g, Mn::BNE, top);
        Emit(g, Mn::LDA, Mode::Abs, high);
        OpA(g, Mn::CMP, f.end, 1);
        Branch(g, Mn::BNE, top);
        Jump(g, f.end_label);
        PlaceLabel(g, wrap);
        next_page();
        Jump(g, check);
    }
    // the derived pointers start where the counter starts, just before the loop
    InitDerived(g, used, init_at, high);
    g.violation = outer;
    return true;
}

void MemoryLoop(Gen& g, const ForParts& f) {
    Opnd v = MemOpnd(SymValue(g.be->var_sym[static_cast<std::size_t>(f.var)]), f.size, 1);
    CopyOpnd(g, f.down ? ImmOpnd(f.trips, 1) : f.start, v);
    std::vector<ReducedLet> reduced = FindReducedLets(g, f.var, f.body);
    InitReducedLets(g, reduced);
    std::vector<ReducedByte> bytes;
    if (f.size == 1) {
        bytes = FindReducedBytes(g, f.var, f.body, reduced);
    }
    InitReducedBytes(g, bytes);
    // a u16 counter indexes through step pointers that move with it (gen_step.cpp)
    std::size_t at = g.out->size();
    std::size_t stepped = f.size == 2 ? BeginStepped(g, f.id, f.var) : g.stepped.size();
    std::int32_t top = NewLabel(g);
    std::int32_t next = NewLabel(g);
    g.loops.back().continue_label = next;
    PlaceLabel(g, top);
    GenStmt(g, f.body);
    PlaceLabel(g, next);
    StepReducedLets(g, reduced);
    DropReducedLets(g, reduced);
    StepReducedBytes(g, bytes);
    DropReducedBytes(g, bytes);
    if (f.down) {
        RmwByte(g, Mn::DEC, v, 0);
        Branch(g, Mn::BNE, top);
        return;
    }
    if (f.size == 1) {
        RmwByte(g, Mn::INC, v, 0);
        if (IsImmZeroByte(f.end, 0)) {
            Branch(g, Mn::BNE, top);
            return;
        }
        LoadA(g, v, 0);
        OpA(g, Mn::CMP, f.end, 0);
        Branch(g, Mn::BNE, top);
        MarkBackEdge(g, f);
        return;
    }
    // the counter's step pointers move with it; the body made all of them by now
    StepOwnPointers(g, f.id, f.var, 1);
    if (IsImmZeroByte(f.end, 0) && IsImmZeroByte(f.end, 1)) {
        RmwByte(g, Mn::INC, v, 0);
        Branch(g, Mn::BNE, top);
        RmwByte(g, Mn::INC, v, 1);
        Branch(g, Mn::BNE, top);
    } else if (IsImmZeroByte(f.end, 0)) {
        // counting up one at a time, the counter can only reach the end at a page start
        RmwByte(g, Mn::INC, v, 0);
        Branch(g, Mn::BNE, top);
        RmwByte(g, Mn::INC, v, 1);
        LoadA(g, v, 1);
        OpA(g, Mn::CMP, f.end, 1);
        Branch(g, Mn::BNE, top);
    } else {
        std::int32_t carry = NewLabel(g);
        RmwByte(g, Mn::INC, v, 0);
        Branch(g, Mn::BNE, carry);
        RmwByte(g, Mn::INC, v, 1);
        PlaceLabel(g, carry);
        LoadA(g, v, 0);
        OpA(g, Mn::CMP, f.end, 0);
        Branch(g, Mn::BNE, top);
        LoadA(g, v, 1);
        OpA(g, Mn::CMP, f.end, 1);
        Branch(g, Mn::BNE, top);
    }
    EndStepped(g, f.id, stepped, at);
}

} // namespace

// the counter in Y (TryWide) or in memory, whichever costs the fewest cycles
void WordLoop(Gen& g, const ForParts& f, bool pin) {
    std::size_t mark = g.out->size();
    Gen start = g;
    // the body's stepped u16 variables, with each choice's code (gen_step.cpp)
    std::size_t stepped = BeginStepped(g, f.id, -1);
    if (!pin || !TryWide(g, f)) {
        MemoryLoop(g, f);
        EndStepped(g, f.id, stepped, mark);
        return;
    }
    EndStepped(g, f.id, stepped, mark);
    std::uint64_t wide_cost = g.cost - start.cost;
    if (g.failed) {
        return;
    }
    std::vector<AsmItem> wide_items(g.out->begin() + static_cast<std::ptrdiff_t>(mark),
                                    g.out->end());
    Gen wide = g;
    g = start;
    g.out->resize(mark);
    stepped = BeginStepped(g, f.id, -1);
    MemoryLoop(g, f);
    EndStepped(g, f.id, stepped, mark);
    if (g.failed || g.cost - start.cost < wide_cost) {
        return;
    }
    g = wide;
    g.out->resize(mark);
    g.out->insert(g.out->end(), wide_items.begin(), wide_items.end());
}

void GenFor(Gen& g, ExprId id) {
    const Program& program = *g.program;
    const Expr& e = ExprOf(program, id);
    ForParts f;
    f.id = id;
    f.var = e.ref;
    TypeId type = program.vars[static_cast<std::size_t>(e.ref)].type;
    TypeKind kind = KindOf(program, type);
    bool is_signed = IsSignedKind(kind);
    f.size = TypeOf(program, type).size;
    f.body = e.c;

    // start, then end, each evaluated once
    f.start = Eval(g, e.a, nullptr);
    if (f.start.in_a || MayWriteOpnd(g, e.b, f.start)) {
        f.start = Snapshot(g, f.start);
    }
    if (e.b >= 0) {
        f.end = Eval(g, e.b, nullptr);
        if (!EndIsStable(g, f.end, e.c)) {
            Opnd t = NewTemp(g, f.end.size);
            CopyOpnd(g, f.end, t);
            f.end = t;
        }
    } else {
        // through the type's largest value: the counter stops where it wraps to
        std::uint32_t past = f.size == 1 ? 0x100 : 0x10000;
        past = is_signed ? past / 2 : past;
        f.end = ImmOpnd(static_cast<std::uint16_t>(past & 0xFFFF), f.size);
    }

    f.end_label = NewLabel(g);
    std::uint16_t cs = 0;
    std::uint16_t ce = 0;
    bool fixed = e.b >= 0 && ConstValue(program, e.a, cs) && ConstValue(program, e.b, ce);
    if (fixed && SignedValue(kind, cs) >= SignedValue(kind, ce)) {
        return;
    }
    std::int32_t trips = fixed ? SignedValue(kind, ce) - SignedValue(kind, cs) : 0;
    if (f.size == 1 && trips >= 1 && trips <= 256 && !ReadsVar(program, f.body, f.var)) {
        f.down = true;
        f.trips = static_cast<std::uint8_t>(trips & 0xFF);
    }
    if (e.b >= 0 && !fixed) {
        CompareJump(g, Op::Ge, is_signed, f.start, f.end, f.end_label);
    }

    LoopCtx ctx;
    ctx.break_label = f.end_label;
    g.loops.push_back(ctx);
    g.loop_factors.resize(static_cast<std::size_t>(g.loop_depth));
    g.loop_factors.push_back(LoopFactor(g, id));
    g.loop_depth++;
    g.for_depth++;
    bool pin = g.for_depth <= kMaxPinnedDepth;
    std::uint32_t copies = f.trips == 0 ? 256 : f.trips;
    bool unroll = f.down && copies <= static_cast<std::uint32_t>(kMaxUnrollTrips) &&
                  CountExprs(program, f.body) * copies <= kMaxUnrollExprs;
    if (unroll) {
        // each copy runs once
        g.loop_factors.back() = 1;
        UnrolledLoop(g, f);
    } else if (f.size == 1) {
        g.ranges.push_back(CounterRangeOf(program, e, kind));
        ByteLoop(g, f, pin);
        g.ranges.pop_back();
    } else {
        WordLoop(g, f, pin);
    }
    g.for_depth--;
    g.loop_depth--;
    g.loops.pop_back();
    PlaceLabel(g, f.end_label);
}

} // namespace tisple
