#include "backend/gen.hpp"

// Strength reduction of a u8 loop's 16-bit lets: a let at the top level of the body whose
// value is the counter times a constant plus things the body never changes, like
// (let row (ptr u8) (row_of cur y)) with row_of returning (+ world (* (as u16 y) 42)), is
// worked out once before the loop and moved by the constant (42) at the end of every
// iteration, instead of being computed again.
//
// The value is followed through additions, constant multiplies and shifts, conversions,
// pointer arithmetic and calls to functions whose body is one such expression (with their
// parameters standing for the arguments). A byte part, like (+ y 1) or (<< r 1), must not
// wrap for any value the counter takes (CounterRange). Everything else in it must be
// invariant: no effects, the counter not read, and no variable it reads written by the
// body. The let's variable is never set, nor its address taken, so it equals the value
// everywhere in its scope: the iteration's value from the top of the iteration on.

namespace tisple {

namespace {

constexpr int kMaxDepth = 8;

// arguments standing for a callee's parameters, and the caller's own context
struct Ctx {
    const Function* fn = nullptr;
    const Expr* call = nullptr;
    const Ctx* outer = nullptr;
};

struct Loop {
    Gen* g = nullptr;
    std::int32_t counter = -1;
    ExprId body = -1;
    int lo = 0;
    int hi = 255;
};

const Expr& At(const Loop& l, ExprId id) {
    return ExprOf(*l.g->program, id);
}

// the argument a parameter stands for, or -1
ExprId ArgumentFor(const Loop& l, const Ctx* ctx, std::int32_t var) {
    if (!ctx || !ctx->fn) {
        return -1;
    }
    for (std::size_t i = 0; i < ctx->fn->params.size(); i++) {
        if (ctx->fn->params[i] == var) {
            return ListItem(*l.g->program, *ctx->call, static_cast<std::uint32_t>(i));
        }
    }
    return -1;
}

// a variable of the callee the context is for (not one of the loop's, nor a global)
bool IsCalleeLocal(const Loop& l, const Ctx* ctx, std::int32_t var) {
    if (!ctx || !ctx->fn) {
        return false;
    }
    std::int32_t owner = l.g->program->vars[static_cast<std::size_t>(var)].function;
    return owner >= 0 && &l.g->program->functions[static_cast<std::size_t>(owner)] == ctx->fn;
}

bool Invariant(const Loop& l, ExprId id, const Ctx* ctx, int depth);

// the same value on every iteration
bool Invariant(const Loop& l, ExprId id, const Ctx* ctx, int depth) {
    if (id < 0) {
        return true;
    }
    if (depth > kMaxDepth) {
        return false;
    }
    const Program& program = *l.g->program;
    const Expr& e = At(l, id);
    switch (e.kind) {
    case ExprKind::Literal:
        return true;
    case ExprKind::VarRef: {
        ExprId arg = ArgumentFor(l, ctx, e.ref);
        if (arg >= 0) {
            return Invariant(l, arg, ctx->outer, depth + 1);
        }
        if (IsCalleeLocal(l, ctx, e.ref)) {
            return false; // set inside the callee
        }
        return e.ref != l.counter && !MayWriteVarIn(*l.g, l.body, e.ref);
    }
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::AddrOf:
        return Invariant(l, e.a, ctx, depth + 1) && Invariant(l, e.b, ctx, depth + 1);
    case ExprKind::Load:
    case ExprKind::LoadField:
        // memory the body may store to is not invariant
        return !HasWrites(program, l.body) && Invariant(l, e.a, ctx, depth + 1);
    default:
        return false;
    }
}

// a byte value counter * coef + c with no wrap: the values it takes, lo to hi
struct ByteLin {
    bool ok = false;
    int coef = 0;
    int lo = 0;
    int hi = 0;
};

ByteLin Byte(const Loop& l, ExprId id, const Ctx* ctx, int depth) {
    ByteLin out;
    if (depth > kMaxDepth || ExprSize(*l.g->program, id) != 1 ||
        IsSignedKind(ExprKindOf(*l.g->program, id))) {
        return out;
    }
    const Expr& e = At(l, id);
    std::uint16_t c = 0;
    if (ConstValue(*l.g->program, id, c)) {
        out.ok = true;
        out.lo = c & 0xFF;
        out.hi = c & 0xFF;
        return out;
    }
    if (e.kind == ExprKind::VarRef) {
        ExprId arg = ArgumentFor(l, ctx, e.ref);
        if (arg >= 0) {
            return Byte(l, arg, ctx->outer, depth + 1);
        }
        if (!IsCalleeLocal(l, ctx, e.ref) && e.ref == l.counter) {
            out.ok = true;
            out.coef = 1;
            out.lo = l.lo;
            out.hi = l.hi;
            return out;
        }
    }
    if (e.kind != ExprKind::Binary) {
        return out;
    }
    ByteLin a = Byte(l, e.a, ctx, depth + 1);
    ByteLin b = Byte(l, e.b, ctx, depth + 1);
    if (!a.ok || !b.ok) {
        return out;
    }
    bool b_const = b.coef == 0 && b.lo == b.hi;
    bool a_const = a.coef == 0 && a.lo == a.hi;
    switch (e.op) {
    case Op::Add:
        out.coef = a.coef + b.coef;
        out.lo = a.lo + b.lo;
        out.hi = a.hi + b.hi;
        break;
    case Op::Sub:
        if (!b_const) {
            return out;
        }
        out.coef = a.coef;
        out.lo = a.lo - b.lo;
        out.hi = a.hi - b.lo;
        break;
    case Op::Mul:
        if (!a_const && !b_const) {
            return out;
        }
        if (a_const) {
            std::swap(a, b);
        }
        out.coef = a.coef * b.lo;
        out.lo = a.lo * b.lo;
        out.hi = a.hi * b.lo;
        break;
    case Op::Shl:
        if (!b_const || b.lo > 7) {
            return out;
        }
        out.coef = a.coef << b.lo;
        out.lo = a.lo << b.lo;
        out.hi = a.hi << b.lo;
        break;
    default:
        return out;
    }
    out.ok = out.lo >= 0 && out.hi <= 0xFF;
    return out;
}

bool Word(const Loop& l, ExprId id, const Ctx* ctx, int depth, std::uint32_t& coef);

// the address of a place, invariant + counter * coef: elements of a packed array (one whose
// address is taken is stored packed) and their fields
bool AddressWord(const Loop& l, ExprId place, const Ctx* ctx, int depth, std::uint32_t& coef) {
    coef = 0;
    if (depth > kMaxDepth) {
        return false;
    }
    const Program& program = *l.g->program;
    const Expr& e = At(l, place);
    switch (e.kind) {
    case ExprKind::VarRef: {
        const Var& var = program.vars[static_cast<std::size_t>(e.ref)];
        return var.address_taken && ArgumentFor(l, ctx, e.ref) < 0 && !IsCalleeLocal(l, ctx, e.ref);
    }
    case ExprKind::Field:
        return AddressWord(l, e.a, ctx, depth + 1, coef);
    case ExprKind::Index: {
        std::uint32_t a = 0;
        std::uint32_t b = 0;
        if (!AddressWord(l, e.a, ctx, depth + 1, a) || !Word(l, e.b, ctx, depth + 1, b)) {
            return false;
        }
        std::uint32_t size = TypeOf(program, e.type).size;
        coef = (a + b * size) & 0xFFFF;
        return true;
    }
    default:
        return false;
    }
}

// a 16-bit value invariant + counter * coef (mod 65536); false if it is not one
bool Word(const Loop& l, ExprId id, const Ctx* ctx, int depth, std::uint32_t& coef) {
    coef = 0;
    if (depth > kMaxDepth) {
        return false;
    }
    const Program& program = *l.g->program;
    if (Invariant(l, id, ctx, depth)) {
        return true;
    }
    const Expr& e = At(l, id);
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint16_t c = 0;
    switch (e.kind) {
    case ExprKind::VarRef: {
        ExprId arg = ArgumentFor(l, ctx, e.ref);
        if (arg < 0 && !IsCalleeLocal(l, ctx, e.ref) && e.ref == l.counter &&
            ExprSize(program, id) == 2) {
            coef = 1; // a u16 counter
            return true;
        }
        return arg >= 0 && Word(l, arg, ctx->outer, depth + 1, coef);
    }
    case ExprKind::AddrOf:
        return AddressWord(l, e.a, ctx, depth + 1, coef);
    case ExprKind::Convert: {
        if (ExprSize(program, e.a) != 1) {
            return Word(l, e.a, ctx, depth + 1, coef);
        }
        ByteLin byte = Byte(l, e.a, ctx, depth + 1);
        coef = static_cast<std::uint32_t>(byte.coef);
        return byte.ok;
    }
    case ExprKind::Reinterpret:
        return Word(l, e.a, ctx, depth + 1, coef);
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
        if (!Word(l, e.a, ctx, depth + 1, a) || !Word(l, e.b, ctx, depth + 1, b)) {
            return false;
        }
        b *= e.value;
        coef = (e.kind == ExprKind::PtrAdd ? a + b : a - b) & 0xFFFF;
        return true;
    case ExprKind::Binary:
        if (e.op == Op::Add || e.op == Op::Sub) {
            if (!Word(l, e.a, ctx, depth + 1, a) || !Word(l, e.b, ctx, depth + 1, b)) {
                return false;
            }
            coef = (e.op == Op::Add ? a + b : a - b) & 0xFFFF;
            return true;
        }
        if (e.op == Op::Mul && ConstValue(program, e.b, c)) {
            if (!Word(l, e.a, ctx, depth + 1, a)) {
                return false;
            }
            coef = (a * c) & 0xFFFF;
            return true;
        }
        if (e.op == Op::Mul && ConstValue(program, e.a, c)) {
            if (!Word(l, e.b, ctx, depth + 1, b)) {
                return false;
            }
            coef = (b * c) & 0xFFFF;
            return true;
        }
        if (e.op == Op::Shl && ConstValue(program, e.b, c) && c < 16) {
            if (!Word(l, e.a, ctx, depth + 1, a)) {
                return false;
            }
            coef = (a << c) & 0xFFFF;
            return true;
        }
        return false;
    case ExprKind::Call: {
        // a callee that is one expression of its parameters
        const Function& fn = program.functions[static_cast<std::size_t>(e.ref)];
        const Expr& body = ExprOf(program, fn.body);
        if (body.kind != ExprKind::Do || body.count != 1) {
            return false;
        }
        for (std::int32_t param : fn.params) {
            if (MayWriteVarIn(*l.g, fn.body, param)) {
                return false;
            }
        }
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (HasEffects(program, ListItem(program, e, i))) {
                return false;
            }
        }
        Ctx inner;
        inner.fn = &fn;
        inner.call = &e;
        inner.outer = ctx;
        return Word(l, ListItem(program, body, 0), &inner, depth + 1, coef);
    }
    default:
        return false;
    }
}

// a set of var anywhere in code
bool SetsVar(const Program& program, ExprId code, std::int32_t var) {
    if (code < 0) {
        return false;
    }
    const Expr& e = ExprOf(program, code);
    if (e.kind == ExprKind::Set) {
        ExprId place = e.a;
        while (place >= 0) {
            const Expr& p = ExprOf(program, place);
            if (p.kind == ExprKind::VarRef) {
                if (p.ref == var) {
                    return true;
                }
                break;
            }
            if (p.kind != ExprKind::Index && p.kind != ExprKind::Field) {
                break;
            }
            place = p.a;
        }
    }
    if (SetsVar(program, e.a, var) || SetsVar(program, e.b, var) || SetsVar(program, e.c, var)) {
        return true;
    }
    if (e.kind == ExprKind::Do || e.kind == ExprKind::Call || e.kind == ExprKind::StructLit) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (SetsVar(program, ListItem(program, e, i), var)) {
                return true;
            }
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (SetsVar(program, program.clauses[e.first + i].body, var)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

std::vector<ReducedLet> FindReducedLets(Gen& g, std::int32_t counter, ExprId body) {
    std::vector<ReducedLet> out;
    const Program& program = *g.program;
    const Expr& b = ExprOf(program, body);
    TypeKind kind = KindOf(program, program.vars[static_cast<std::size_t>(counter)].type);
    if (b.kind != ExprKind::Do || (kind != TypeKind::U8 && kind != TypeKind::U16)) {
        return out;
    }
    Loop l;
    l.g = &g;
    l.counter = counter;
    l.body = body;
    for (const CounterRange& r : g.ranges) {
        if (r.var == counter) {
            l.lo = r.lo;
            l.hi = r.hi;
        }
    }
    for (std::uint32_t i = 0; i < b.count; i++) {
        ExprId item = ListItem(program, b, i);
        const Expr& e = ExprOf(program, item);
        if (e.kind != ExprKind::Let || e.a < 0 || ExprSize(program, e.a) != 2) {
            continue;
        }
        const Var& var = program.vars[static_cast<std::size_t>(e.ref)];
        const IndexAlias& alias = g.be->aliases[static_cast<std::size_t>(e.ref)];
        if (var.address_taken || alias.index_only || alias.forward ||
            SetsVar(program, body, e.ref)) {
            continue;
        }
        std::uint32_t coef = 0;
        if (!Word(l, e.a, nullptr, 0, coef) || coef == 0) {
            continue;
        }
        ReducedLet reduced;
        reduced.var = e.ref;
        reduced.value = e.a;
        reduced.step = static_cast<std::uint16_t>(coef);
        out.push_back(reduced);
    }
    return out;
}

bool LoopInvariant(Gen& g, std::int32_t counter, ExprId body, ExprId id) {
    Loop l;
    l.g = &g;
    l.counter = counter;
    l.body = body;
    return Invariant(l, id, nullptr, 0);
}

void InitReducedLets(Gen& g, const std::vector<ReducedLet>& lets) {
    for (const ReducedLet& r : lets) {
        SourceNote(g, g.program->vars[static_cast<std::size_t>(r.var)].loc);
        AsmValue base = SymValue(g.be->var_sym[static_cast<std::size_t>(r.var)]);
        EvalInto(g, r.value, MemOpnd(base, 2, 1));
        g.reduced.push_back(r.var);
    }
}

void StepReducedLets(Gen& g, const std::vector<ReducedLet>& lets) {
    for (const ReducedLet& r : lets) {
        SourceNote(g, g.program->vars[static_cast<std::size_t>(r.var)].loc);
        AsmValue low = SymValue(g.be->var_sym[static_cast<std::size_t>(r.var)]);
        Opnd v = MemOpnd(low, 2, 1);
        if (r.step >= 0x100) {
            Combine16(g, Op::Add, v, ImmOpnd(r.step, 2), v);
            continue;
        }
        // a step below 256 only reaches the high byte on a carry
        std::int32_t skip = NewLabel(g);
        if (r.step == 1) {
            Emit(g, Mn::INC, Mode::Abs, low);
            Branch(g, Mn::BNE, skip);
            Emit(g, Mn::INC, Mode::Abs, SymValue(low.sym, low.offset + 1));
            PlaceLabel(g, skip);
            continue;
        }
        Emit(g, Mn::LDA, Mode::Abs, low);
        Emit(g, Mn::CLC, Mode::Imp);
        Emit(g, Mn::ADC, Mode::Imm, Num(r.step));
        Emit(g, Mn::STA, Mode::Abs, low);
        Branch(g, Mn::BCC, skip);
        Emit(g, Mn::INC, Mode::Abs, SymValue(low.sym, low.offset + 1));
        PlaceLocalLabel(g, skip);
    }
}

void DropReducedLets(Gen& g, const std::vector<ReducedLet>& lets) {
    g.reduced.resize(g.reduced.size() - lets.size());
}

} // namespace tisple
