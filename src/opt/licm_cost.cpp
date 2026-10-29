// What moving an expression out of a loop wins: a rough count of the cycles the 6502 spends
// on it, how many times a loop runs when the tree says, and work the backend does faster
// inside loops than outside them.

#include "check/arith.hpp"
#include "opt/licm.hpp"

namespace tisple::opt {

namespace {

// cycles an expression must save over reading the local it moves to
constexpr std::int32_t kMinSaving = 12;
// a multiply by a constant with at most this many bits set is shifts and adds, inline
constexpr int kInlineMulBits = 4;

bool Wide(const Pass& p, const Expr& e) {
    return TypeOf(*p.program, e.type).size > 1;
}

bool ConstPowerOfTwo(const Pass& p, ExprId id) {
    std::uint16_t c = 0;
    return ConstValue(*p.program, id, c) && c != 0 && (c & (c - 1)) == 0;
}

int Popcount(std::uint16_t v) {
    int n = 0;
    for (; v != 0; v = static_cast<std::uint16_t>(v & (v - 1))) {
        n++;
    }
    return n;
}

// a multiply the backend calls a routine for: by a variable, or a constant with many bits
bool RoutineMultiply(const Pass& p, const Expr& e) {
    if (e.kind != ExprKind::Binary || e.op != Op::Mul) {
        return false;
    }
    std::uint16_t c = 0;
    bool constant = ConstValue(*p.program, e.b, c) || ConstValue(*p.program, e.a, c);
    return !constant || Popcount(c) > kInlineMulBits;
}

std::int32_t Cost(const Pass& p, ExprId id);

std::int32_t BinaryCost(const Pass& p, const Expr& e) {
    const Expr& a = At(p, e.a);
    bool wide = Wide(p, a);
    std::int32_t operands = Cost(p, e.a) + Cost(p, e.b);
    std::uint16_t c = 0;
    bool is_signed = IsSignedKind(KindOf(*p.program, a.type));
    switch (e.op) {
    case Op::Mul:
        if (ConstPowerOfTwo(p, e.a) || ConstPowerOfTwo(p, e.b)) {
            return operands + (wide ? 12 : 4);
        }
        return operands + (!RoutineMultiply(p, e) ? 20 : wide ? 250 : 80);
    case Op::Div:
    case Op::Mod:
        if (ConstPowerOfTwo(p, e.b)) {
            return operands + (is_signed ? 20 : wide ? 12 : 4);
        }
        // an unsigned byte by 128 or more: one compare
        if (!is_signed && !wide && ConstValue(*p.program, e.b, c) && c >= 128) {
            return operands + 6;
        }
        return operands + (wide ? 400 : 150);
    case Op::Shl:
    case Op::Shr:
        if (ConstValue(*p.program, e.b, c)) {
            // shifted out entirely: nothing to work out
            std::int32_t bits = wide ? 16 : 8;
            return operands + (c >= bits ? 2 : c * (wide ? 5 : 2));
        }
        return operands + (wide ? 60 : 30);
    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
        return operands + (wide ? 10 : 4);
    default:
        return operands + (wide ? 8 : 3);
    }
}

std::int32_t Cost(const Pass& p, ExprId id) {
    if (id < 0) {
        return 0;
    }
    const Expr& e = At(p, id);
    std::int32_t wide = Wide(p, e) ? 2 : 1;
    switch (e.kind) {
    case ExprKind::Literal:
        return 0;
    case ExprKind::VarRef:
        return 3 * wide;
    case ExprKind::Index:
        return Cost(p, e.a) + Cost(p, e.b) + 4 * wide + (Wide(p, At(p, e.b)) ? 10 : 0);
    case ExprKind::Field:
        return Cost(p, e.a) + 2;
    case ExprKind::AddrOf:
        return Cost(p, e.a) + 4;
    case ExprKind::Unary:
        return Cost(p, e.a) + 2 * wide;
    case ExprKind::Convert:
        return Cost(p, e.a) + 4;
    case ExprKind::Reinterpret:
        return Cost(p, e.a);
    case ExprKind::Binary:
        return BinaryCost(p, e);
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
        return Cost(p, e.a) + Cost(p, e.b) + (e.value > 1 ? 20 : 10);
    case ExprKind::And:
    case ExprKind::Or:
    case ExprKind::If:
        return Cost(p, e.a) + Cost(p, e.b) + Cost(p, e.c) + 4;
    case ExprKind::Call: {
        std::int32_t cost = 40;
        for (std::uint32_t i = 0; i < e.count; i++) {
            cost += Cost(p, Item(p, e, i));
        }
        return cost;
    }
    default:
        return 0;
    }
}

// how many times a for or while counted by c runs: the step of c's one write in the loop,
// (set c (+ c s)) among the body's statements, 0 when there is not exactly one such write
std::int64_t StepOfCounter(const Pass& p, const Expr& loop, std::int32_t c) {
    const Expr& body = At(p, loop.b);
    if (body.kind != ExprKind::Do || CountSets(p, loop.b, c) != 1) {
        return 0;
    }
    for (std::uint32_t i = 0; i < body.count; i++) {
        const Expr& set = At(p, Item(p, body, i));
        if (set.kind != ExprKind::Set || At(p, set.a).kind != ExprKind::VarRef ||
            At(p, set.a).ref != c) {
            continue;
        }
        const Expr& add = At(p, set.b);
        std::uint16_t s = 0;
        bool step = add.kind == ExprKind::Binary && add.op == Op::Add &&
                    At(p, add.a).kind == ExprKind::VarRef && At(p, add.a).ref == c &&
                    ConstValue(*p.program, add.b, s);
        TypeKind kind = KindOf(*p.program, add.type);
        return step ? SignedValue(kind, s) : 0;
    }
    return 0;
}

} // namespace

std::uint32_t CountSets(const Pass& p, ExprId id, std::int32_t var) {
    if (id < 0) {
        return 0;
    }
    const Expr& e = At(p, id);
    std::uint32_t count = 0;
    if (e.kind == ExprKind::Set && PlaceRoot(p, e.a) == var) {
        count++;
    }
    if ((e.kind == ExprKind::Let || e.kind == ExprKind::For) && e.ref == var) {
        count++;
    }
    EachChild(p, e, [&](ExprId child) { count += CountSets(p, child, var); });
    return count;
}

bool Worth(const Pass& p, ExprId id) {
    const Expr& e = At(p, id);
    // a condition branches on its compare directly; a bool in a local would be made, then
    // tested (its operands may still move)
    TypeKind kind = KindOf(*p.program, e.type);
    if (!IsScalarKind(kind) || kind == TypeKind::Bool) {
        return false;
    }
    // a byte widened: the backend knows its high byte is zero (an index register, (p),Y),
    // which a word in a local would hide; the byte may move
    if (e.kind == ExprKind::Convert && Wide(p, e) && !Wide(p, At(p, e.a))) {
        return false;
    }
    return Cost(p, id) - 3 * (Wide(p, e) ? 2 : 1) >= kMinSaving;
}

bool FasterInLoops(const Pass& p, ExprId id) {
    if (id < 0) {
        return false;
    }
    const Expr& e = At(p, id);
    if (RoutineMultiply(p, e)) {
        return true;
    }
    bool faster = false;
    EachChild(p, e, [&](ExprId child) { faster = faster || FasterInLoops(p, child); });
    return faster;
}

std::int64_t KnownTrips(const Pass& p, ExprId loop, ExprId before) {
    const Expr& e = At(p, loop);
    std::uint16_t start = 0;
    std::uint16_t end = 0;
    if (e.kind == ExprKind::For) {
        TypeKind kind = KindOf(*p.program, At(p, e.a).type);
        if (!ConstValue(*p.program, e.a, start)) {
            return -1;
        }
        // to one past the type's max, as the interpreter runs it
        std::int64_t last = (KindBits(kind) == 8 ? 256 : 65536) / (IsSignedKind(kind) ? 2 : 1);
        if (e.b >= 0 && !ConstValue(*p.program, e.b, end)) {
            return -1;
        }
        if (e.b >= 0) {
            last = SignedValue(kind, end);
        }
        std::int64_t first = SignedValue(kind, start);
        return last > first ? last - first : 0;
    }
    // (let c v) (while (< c k) ... (set c (+ c s)) ...), v, k and s literals
    const Expr& cond = At(p, e.a);
    if (before < 0 || cond.kind != ExprKind::Binary || cond.op != Op::Lt ||
        At(p, cond.a).kind != ExprKind::VarRef || !ConstValue(*p.program, cond.b, end)) {
        return -1;
    }
    std::int32_t c = At(p, cond.a).ref;
    const Expr& let = At(p, before);
    if (let.kind != ExprKind::Let || let.ref != c || !ConstValue(*p.program, let.a, start)) {
        return -1;
    }
    std::int64_t step = StepOfCounter(p, e, c);
    TypeKind kind = KindOf(*p.program, At(p, cond.a).type);
    std::int64_t from = SignedValue(kind, start);
    std::int64_t to = SignedValue(kind, end);
    // a step that could wrap past the bound is not counted
    std::int64_t top = (KindBits(kind) == 8 ? 256 : 65536) / (IsSignedKind(kind) ? 2 : 1);
    if (step <= 0 || to - 1 + step >= top || ContinuesHere(p, e.b, 0)) {
        return -1;
    }
    return from >= to ? 0 : (to - from + step - 1) / step;
}

bool BodyRuns(const Pass& p, ExprId loop) {
    const Expr& e = At(p, loop);
    std::uint16_t cond = 0;
    if (e.kind == ExprKind::While) {
        return ConstValue(*p.program, e.a, cond) && cond != 0;
    }
    return KnownTrips(p, loop, -1) > 0;
}

} // namespace tisple::opt
