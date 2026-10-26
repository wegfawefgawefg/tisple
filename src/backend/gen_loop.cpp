#include "backend/gen.hpp"
#include "check/arith.hpp"

#include <algorithm>

namespace tisple {

namespace {

// a u8 counter nests this deep at most in registers (each level tries X, Y, memory)
constexpr int kMaxPinnedDepth = 3;

struct ForParts {
    std::int32_t var = -1;
    std::uint32_t size = 1;
    Opnd start;
    Opnd end; // raw bits the counter stops at: Imm or plain memory
    ExprId body = -1;
    std::int32_t end_label = -1;
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

// the counter in register r; false (and nothing kept) if the body needed r
bool TryPinned(Gen& g, const ForParts& f, int r) {
    std::size_t mark = g.out->size();
    Gen saved = g;
    bool outer = g.violation;
    g.violation = false;
    LoadCounter(g, r, f.start);
    g.pin[r] = f.var;
    std::int32_t top = NewLabel(g);
    std::int32_t next = NewLabel(g);
    g.loops.back().continue_label = next;
    PlaceLabel(g, top);
    GenStmt(g, f.body);
    PlaceLabel(g, next);
    g.pin[r] = -1;
    if (g.violation && !g.failed) {
        g.out->resize(mark);
        g = saved;
        return false;
    }
    Emit(g, r == 0 ? Mn::INX : Mn::INY, Mode::Imp);
    if (!IsImmZeroByte(f.end, 0)) {
        const ByteRef& b = f.end.b[0];
        Emit(g, r == 0 ? Mn::CPX : Mn::CPY, b.kind == ByteKind::Imm ? Mode::Imm : Mode::Abs,
             b.value);
    }
    Branch(g, Mn::BNE, top);
    g.violation = outer;
    return true;
}

// the cycles of the code from item `first` on, as written (no loop counts)
std::uint64_t StaticCycles(const Gen& g, std::size_t first) {
    std::uint64_t total = 0;
    for (std::size_t i = first; i < g.out->size(); i++) {
        total += static_cast<std::uint64_t>(ItemCycles(g.be->as, (*g.out)[i]));
    }
    return total;
}

// the counter in X or in Y, whichever body comes out cheaper; false if neither works
bool PinCheapest(Gen& g, const ForParts& f) {
    std::size_t mark = g.out->size();
    Gen start = g;
    bool found = false;
    std::uint64_t best_cost = 0;
    std::vector<AsmItem> best_items;
    Gen best = g;
    for (int r = 0; r < 2; r++) {
        if (start.pin[r] >= 0) {
            continue;
        }
        g = start;
        g.out->resize(mark);
        if (!TryPinned(g, f, r)) {
            continue;
        }
        std::uint64_t cost = StaticCycles(g, mark);
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
    g = found ? best : start;
    g.out->resize(mark);
    g.out->insert(g.out->end(), best_items.begin(), best_items.end());
    return found;
}

bool SameBase(const Derived& d, const AsmValue& base, std::int32_t var, std::int32_t offset) {
    return d.var == var && d.offset == offset && d.base.sym == base.sym &&
           d.base.offset == base.offset && d.base.part == base.part;
}

// T = base + offset + (high << 8): only A and the carry
void InitDerived(Gen& g, const Derived& d, const AsmValue& high) {
    Opnd t = MemOpnd(d.ptr, 2, 1);
    Opnd base;
    if (d.var >= 0) {
        base = MemOpnd(d.base, 2, 1);
    } else {
        AsmValue address = d.base;
        address.offset = (address.offset + d.offset) & 0xFFFF;
        base = d.base.sym >= 0 ? SymImmOpnd(address, 2)
                               : ImmOpnd(static_cast<std::uint16_t>(address.offset), 2);
    }
    if (d.var >= 0 && d.offset != 0) {
        Combine16(g, Op::Add, base, ImmOpnd(static_cast<std::uint16_t>(d.offset & 0xFFFF), 2), t);
        base = t;
    }
    LoadA(g, base, 0);
    StoreA(g, t, 0);
    LoadA(g, base, 1);
    Emit(g, Mn::CLC, Mode::Imp);
    Emit(g, Mn::ADC, Mode::Abs, high);
    StoreA(g, t, 1);
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
    std::int32_t top = NewLabel(g);
    std::int32_t next = NewLabel(g);
    g.loops.back().continue_label = next;
    PlaceLabel(g, top);
    GenStmt(g, f.body);
    PlaceLabel(g, next);
    g.pin[1] = -1;
    g.wide = nullptr;
    if (g.violation && !g.failed) {
        g.out->resize(mark);
        g = saved;
        return false;
    }
    // derived pointers only a discarded trial asked for are not kept up
    std::vector<Derived> used;
    for (const Derived& d : wide.derived) {
        bool referenced = false;
        for (std::size_t i = init_at; i < g.out->size() && !referenced; i++) {
            const AsmItem& item = (*g.out)[i];
            referenced = item.kind == ItemKind::Ins && item.value.sym == d.ptr.sym &&
                         item.value.offset == d.ptr.offset;
        }
        if (referenced) {
            used.push_back(d);
        }
    }
    wide.derived = used;
    // INY; a new page moves the high byte and every derived pointer
    Emit(g, Mn::INY, Mode::Imp);
    auto next_page = [&]() {
        for (const Derived& d : wide.derived) {
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
    std::size_t before = g.out->size();
    for (const Derived& d : wide.derived) {
        InitDerived(g, d, high);
    }
    std::rotate(g.out->begin() + static_cast<std::ptrdiff_t>(init_at),
                g.out->begin() + static_cast<std::ptrdiff_t>(before), g.out->end());
    g.violation = outer;
    return true;
}

void MemoryLoop(Gen& g, const ForParts& f) {
    Opnd v = MemOpnd(SymValue(g.be->var_sym[static_cast<std::size_t>(f.var)]), f.size, 1);
    CopyOpnd(g, f.start, v);
    std::int32_t top = NewLabel(g);
    std::int32_t next = NewLabel(g);
    g.loops.back().continue_label = next;
    PlaceLabel(g, top);
    GenStmt(g, f.body);
    PlaceLabel(g, next);
    if (f.size == 1) {
        RmwByte(g, Mn::INC, v, 0);
        if (!IsImmZeroByte(f.end, 0)) {
            LoadA(g, v, 0);
            OpA(g, Mn::CMP, f.end, 0);
        }
        Branch(g, Mn::BNE, top);
        return;
    }
    if (IsImmZeroByte(f.end, 0) && IsImmZeroByte(f.end, 1)) {
        RmwByte(g, Mn::INC, v, 0);
        Branch(g, Mn::BNE, top);
        RmwByte(g, Mn::INC, v, 1);
        Branch(g, Mn::BNE, top);
        return;
    }
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

} // namespace

AsmValue DerivedPointer(Gen& g, AsmValue base, std::int32_t var, std::int32_t offset) {
    for (const Derived& d : g.wide->derived) {
        if (SameBase(d, base, var, offset)) {
            return d.ptr;
        }
    }
    Derived d;
    d.base = base;
    d.var = var;
    d.offset = offset;
    std::uint32_t at = g.wide->next_slot;
    g.wide->next_slot += 2;
    std::uint32_t& max = g.be->lptr_size[static_cast<std::size_t>(g.function)];
    max = g.wide->next_slot > max ? g.wide->next_slot : max;
    d.ptr = SymValue(g.be->lptr_sym[static_cast<std::size_t>(g.function)],
                     static_cast<std::int32_t>(at));
    g.wide->derived.push_back(d);
    return d.ptr;
}

void GenFor(Gen& g, const Expr& e) {
    const Program& program = *g.program;
    ForParts f;
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
    if (e.b >= 0) {
        std::uint16_t cs = 0;
        std::uint16_t ce = 0;
        if (ConstValue(program, e.a, cs) && ConstValue(program, e.b, ce)) {
            if (SignedValue(kind, cs) >= SignedValue(kind, ce)) {
                return;
            }
        } else {
            CompareJump(g, Op::Ge, is_signed, f.start, f.end, f.end_label);
        }
    }

    LoopCtx ctx;
    ctx.break_label = f.end_label;
    g.loops.push_back(ctx);
    g.loop_depth++;
    g.for_depth++;
    bool done = false;
    if (f.size == 1 && g.for_depth <= kMaxPinnedDepth) {
        done = PinCheapest(g, f);
    } else if (f.size == 2 && g.for_depth <= kMaxPinnedDepth) {
        done = TryWide(g, f);
    }
    if (!done) {
        MemoryLoop(g, f);
    }
    g.for_depth--;
    g.loop_depth--;
    g.loops.pop_back();
    PlaceLabel(g, f.end_label);
}

void GenWhile(Gen& g, const Expr& e) {
    std::uint16_t c = 0;
    bool fixed = ConstValue(*g.program, e.a, c);
    if (fixed && c == 0) {
        return;
    }
    std::int32_t top = NewLabel(g);
    std::int32_t test = NewLabel(g);
    std::int32_t end = NewLabel(g);
    LoopCtx ctx;
    ctx.break_label = end;
    ctx.continue_label = fixed ? top : test;
    g.loops.push_back(ctx);
    g.loop_depth++;
    if (!fixed) {
        Jump(g, test);
    }
    PlaceLabel(g, top);
    GenStmt(g, e.b);
    if (fixed) {
        Jump(g, top);
    } else {
        PlaceLabel(g, test);
        GenBranch(g, e.a, true, top);
    }
    g.loop_depth--;
    g.loops.pop_back();
    PlaceLabel(g, end);
}

} // namespace tisple
