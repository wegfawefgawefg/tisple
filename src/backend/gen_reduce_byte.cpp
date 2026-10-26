#include "backend/gen.hpp"

// Strength reduction and hoisting of a u8 loop's byte expressions. A byte expression in the
// body that is the counter times a constant plus things the body never changes, like
// plasma's (- (* x 3) (<< t 1)), is worked out once before the loop into a temporary and
// moved by the constant at the end of every iteration; with no counter in it, it is only
// worked out once. Byte arithmetic wraps, so adding the step is exact for any values: the
// expression is followed through +, -, constant multiplies and shifts, and conversions
// between u8 and i8. Code reading the expression reads the temporary (TrySimple).
//
// It must be worth it: the expression's code, estimated, costs more than reading the
// temporary and stepping it. Everything in it must be invariant (gen_reduce.cpp) and safe to
// read before the loop, which may not run at all: no loads through pointers and no arrays at
// fixed addresses, which may be I/O.

namespace tisple {

namespace {

constexpr int kMaxDepth = 8;
// LDA temp, or LDY temp where the value is an index
constexpr int kReadCost = 3;
constexpr int kMinGain = 2;

struct ByteLoop {
    Gen* g = nullptr;
    std::int32_t counter = -1;
    ExprId body = -1;
};

const Expr& At(const ByteLoop& l, ExprId id) {
    return ExprOf(*l.g->program, id);
}

bool IsByteInt(const ByteLoop& l, ExprId id) {
    TypeKind kind = ExprKindOf(*l.g->program, id);
    return ExprSize(*l.g->program, id) == 1 && (kind == TypeKind::U8 || kind == TypeKind::I8);
}

// reading it early cannot touch I/O: no loads through pointers, no arrays at fixed addresses
bool SafeEarly(const ByteLoop& l, ExprId id) {
    if (id < 0) {
        return true;
    }
    const Program& program = *l.g->program;
    const Expr& e = At(l, id);
    if (e.kind == ExprKind::Load || e.kind == ExprKind::LoadField || e.kind == ExprKind::IoRead) {
        return false;
    }
    if (e.kind == ExprKind::VarRef &&
        program.vars[static_cast<std::size_t>(e.ref)].placement.has_address) {
        return false;
    }
    if (e.kind != ExprKind::Literal && e.kind != ExprKind::VarRef && e.kind != ExprKind::Index &&
        e.kind != ExprKind::Field && e.kind != ExprKind::Unary && e.kind != ExprKind::Binary &&
        e.kind != ExprKind::Convert) {
        return false;
    }
    return SafeEarly(l, e.a) && SafeEarly(l, e.b);
}

// id as counter * coef + something the loop never changes, mod 256
bool Affine(const ByteLoop& l, ExprId id, int depth, std::uint8_t& coef) {
    coef = 0;
    if (depth > kMaxDepth || !IsByteInt(l, id)) {
        return false;
    }
    if (LoopInvariant(*l.g, l.counter, l.body, id)) {
        return true;
    }
    const Program& program = *l.g->program;
    const Expr& e = At(l, id);
    std::uint8_t a = 0;
    std::uint8_t b = 0;
    std::uint16_t c = 0;
    switch (e.kind) {
    case ExprKind::VarRef:
        if (e.ref != l.counter) {
            return false;
        }
        coef = 1;
        return true;
    case ExprKind::Convert:
        return IsByteInt(l, e.a) && Affine(l, e.a, depth + 1, coef);
    case ExprKind::Binary:
        break;
    default:
        return false;
    }
    switch (e.op) {
    case Op::Add:
    case Op::Sub:
        if (!Affine(l, e.a, depth + 1, a) || !Affine(l, e.b, depth + 1, b)) {
            return false;
        }
        coef = static_cast<std::uint8_t>(e.op == Op::Add ? a + b : a - b);
        return true;
    case Op::Mul:
        if (ConstValue(program, e.b, c) && Affine(l, e.a, depth + 1, a)) {
            coef = static_cast<std::uint8_t>(a * c);
            return true;
        }
        if (ConstValue(program, e.a, c) && Affine(l, e.b, depth + 1, b)) {
            coef = static_cast<std::uint8_t>(b * c);
            return true;
        }
        return false;
    case Op::Shl:
        if (!ConstValue(program, e.b, c) || c > 7 || !Affine(l, e.a, depth + 1, a)) {
            return false;
        }
        coef = static_cast<std::uint8_t>(a << c);
        return true;
    default:
        return false;
    }
}

bool IsLeaf(const ByteLoop& l, ExprId id) {
    ExprKind kind = At(l, id).kind;
    return kind == ExprKind::Literal || kind == ExprKind::VarRef;
}

// about the cycles the expression's code takes each time (gen_arith.cpp's shapes)
int Cost(const ByteLoop& l, ExprId id, int depth) {
    if (depth > kMaxDepth) {
        return 20;
    }
    const Program& program = *l.g->program;
    const Expr& e = At(l, id);
    std::uint16_t c = 0;
    switch (e.kind) {
    case ExprKind::Literal:
        return 2;
    case ExprKind::VarRef:
        return e.ref == l.counter ? 2 : 3;
    case ExprKind::Index:
        // a computed index goes to a register first
        return 4 + (IsLeaf(l, e.b) ? 3 : Cost(l, e.b, depth + 1) + 2);
    case ExprKind::Field:
        return At(l, e.a).kind == ExprKind::VarRef ? 4 : Cost(l, e.a, depth + 1);
    case ExprKind::Convert:
    case ExprKind::Unary:
        return Cost(l, e.a, depth + 1) + (e.kind == ExprKind::Unary ? 2 : 0);
    case ExprKind::Binary:
        break;
    default:
        return 20;
    }
    int a = Cost(l, e.a, depth + 1);
    switch (e.op) {
    case Op::Shl:
        return ConstValue(program, e.b, c) ? a + 2 * (c & 7) : 20;
    case Op::Mul: {
        ExprId other = e.a;
        if (!ConstValue(program, e.b, c)) {
            if (!ConstValue(program, e.a, c)) {
                return 40;
            }
            other = e.b;
        }
        int bits = 0;
        int ones = 0;
        for (std::uint32_t v = c & 0xFF; v != 0; v >>= 1) {
            bits++;
            ones += static_cast<int>(v & 1);
        }
        if (ones > 4) {
            return 100;
        }
        return Cost(l, other, depth + 1) + 2 * (bits - 1) + 5 * (ones - 1) + (ones > 1 ? 3 : 0);
    }
    default:
        // A op b: an operand that needs code goes through a temporary
        return a + 5 + (IsLeaf(l, e.b) ? 1 : Cost(l, e.b, depth + 1) + 6);
    }
}

int StepCost(std::uint8_t coef) {
    if (coef == 0) {
        return 0;
    }
    return coef == 1 || coef == 0xFF ? 5 : 10;
}

void Collect(const ByteLoop& l, ExprId id, const std::vector<ExprId>& skip,
             std::vector<ReducedByte>& out) {
    if (id < 0) {
        return;
    }
    for (ExprId s : skip) {
        if (s == id) {
            return;
        }
    }
    const Program& program = *l.g->program;
    const Expr& e = At(l, id);
    bool arith = e.kind == ExprKind::Binary &&
                 (e.op == Op::Add || e.op == Op::Sub || e.op == Op::Mul || e.op == Op::Shl);
    std::uint8_t coef = 0;
    if (arith && Affine(l, id, 0, coef) && SafeEarly(l, id) &&
        Cost(l, id, 0) > kReadCost + StepCost(coef) + kMinGain) {
        ReducedByte r;
        r.expr = id;
        r.step = coef;
        out.push_back(r);
        return;
    }
    Collect(l, e.a, skip, out);
    Collect(l, e.b, skip, out);
    Collect(l, e.c, skip, out);
    if (e.kind == ExprKind::Do || e.kind == ExprKind::Call || e.kind == ExprKind::StructLit) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            Collect(l, ListItem(program, e, i), skip, out);
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            Collect(l, program.clauses[e.first + i].body, skip, out);
        }
    }
}

} // namespace

std::vector<ReducedByte> FindReducedBytes(Gen& g, std::int32_t counter, ExprId body,
                                          const std::vector<ReducedLet>& lets) {
    std::vector<ReducedByte> out;
    TypeKind kind = KindOf(*g.program, g.program->vars[static_cast<std::size_t>(counter)].type);
    if (kind != TypeKind::U8 && kind != TypeKind::I8) {
        return out;
    }
    ByteLoop l;
    l.g = &g;
    l.counter = counter;
    l.body = body;
    // the reduced lets' values are only worked out before the loop
    std::vector<ExprId> skip;
    for (const ReducedLet& r : lets) {
        skip.push_back(r.value);
    }
    Collect(l, body, skip, out);
    return out;
}

void InitReducedBytes(Gen& g, std::vector<ReducedByte>& bytes) {
    for (ReducedByte& r : bytes) {
        EvalA(g, r.expr);
        r.temp = NewTemp(g, 1);
        StoreA(g, r.temp, 0);
    }
    // only now: their values above are worked out from the expressions themselves
    for (const ReducedByte& r : bytes) {
        g.reduced_bytes.push_back(r);
    }
}

void StepReducedBytes(Gen& g, const std::vector<ReducedByte>& bytes) {
    for (const ReducedByte& r : bytes) {
        if (r.step == 0) {
            continue;
        }
        if (r.step == 1 || r.step == 0xFF) {
            RmwByte(g, r.step == 1 ? Mn::INC : Mn::DEC, r.temp, 0);
            continue;
        }
        LoadA(g, r.temp, 0);
        Emit(g, Mn::CLC, Mode::Imp);
        Emit(g, Mn::ADC, Mode::Imm, Num(r.step));
        StoreA(g, r.temp, 0);
    }
}

void DropReducedBytes(Gen& g, const std::vector<ReducedByte>& bytes) {
    g.reduced_bytes.resize(g.reduced_bytes.size() - bytes.size());
}

} // namespace tisple
