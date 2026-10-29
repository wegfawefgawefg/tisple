// What moving an expression out of a loop wins: a rough count of the cycles the 6502 spends
// on it, and how many times a loop runs when the tree says.

#include "check/arith.hpp"
#include "opt/licm.hpp"

namespace tisple::opt {

namespace {

// cycles an expression must save over reading the local it moves to
constexpr std::int32_t kMinSaving = 12;
// a 16-bit multiply by a constant of at most this many nonzero digits is inline
// (gen_mulconst.cpp); a byte multiply by a constant always is
constexpr int kInlineMulDigits = 4;

bool Wide(const Pass& p, const Expr& e) {
    return TypeOf(*p.program, e.type).size > 1;
}

bool ConstPowerOfTwo(const Pass& p, ExprId id) {
    std::uint16_t c = 0;
    return ConstValue(*p.program, id, c) && c != 0 && (c & (c - 1)) == 0;
}

// a constant's digits as gen_mulconst.cpp picks them: its binary ones, or its signed digits
// (the non-adjacent form) when they do less
struct Digits {
    int top = -1; // the highest nonzero digit
    int nonzero = 0;
    bool negative_top = false;
};

Digits BinaryDigits(std::uint32_t c, int bits) {
    Digits d;
    for (int i = 0; i < bits; i++) {
        if (((c >> i) & 1) != 0) {
            d.top = i;
            d.nonzero++;
        }
    }
    return d;
}

Digits SignedDigits(std::uint32_t c, int bits) {
    Digits d;
    std::int64_t v = c & ((1u << bits) - 1);
    for (int i = 0; i < bits && v != 0; i++) {
        if ((v & 1) != 0) {
            int digit = (v & 3) == 1 ? 1 : -1;
            v -= digit;
            d.top = i;
            d.nonzero++;
            d.negative_top = digit < 0;
        }
        v >>= 1;
    }
    return d;
}

// a byte multiply in A: a shift per digit below the top, an add or subtract per other digit
std::int32_t ByteDigitCycles(const Digits& d) {
    if (d.top < 0) {
        return 0;
    }
    return 2 * d.top + 5 * (d.nonzero - 1) + (d.negative_top ? 6 : 0);
}

std::int32_t ByteMulCycles(std::uint32_t c) {
    return std::min(ByteDigitCycles(BinaryDigits(c, 8)), ByteDigitCycles(SignedDigits(c, 8)));
}

// the digits a 16-bit multiply uses: the signed ones when fewer and the top one is 1
Digits WordDigits(std::uint32_t c) {
    Digits plain = BinaryDigits(c, 16);
    Digits signed_digits = SignedDigits(c, 16);
    bool fewer = signed_digits.nonzero < plain.nonzero;
    return signed_digits.top >= 0 && !signed_digits.negative_top && fewer ? signed_digits : plain;
}

// a multiply's cycles past its operands: by a constant, its shifts and adds inline (a word
// shifts in memory, 10 cycles a bit, and adds 20 a digit), else the routine
std::int32_t MulCycles(const Pass& p, const Expr& e, bool wide) {
    std::uint16_t c = 0;
    bool constant = ConstValue(*p.program, e.b, c) || ConstValue(*p.program, e.a, c);
    if (!constant) {
        return wide ? 250 : 80;
    }
    if (!wide) {
        return ByteMulCycles(c) + 3;
    }
    Digits d = WordDigits(c);
    if (d.nonzero > kInlineMulDigits) {
        return 250;
    }
    return d.top < 0 ? 0 : 10 * d.top + 20 * (d.nonzero - 1) + 12;
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
        return operands + MulCycles(p, e, wide);
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

std::int32_t EstimatedCycles(const Pass& p, ExprId id) {
    return Cost(p, id);
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

ExprId CounterBefore(const Pass& p, ExprId list, std::uint32_t index) {
    if (index == 0) {
        return -1;
    }
    const Expr& d = At(p, list);
    const Expr& loop = At(p, Item(p, d, index));
    ExprId before = Item(p, d, index - 1);
    if (loop.kind != ExprKind::While) {
        return before;
    }
    const Expr& cond = At(p, loop.a);
    if (cond.kind != ExprKind::Binary || At(p, cond.a).kind != ExprKind::VarRef) {
        return before;
    }
    // lets of other variables cannot write the counter
    for (std::uint32_t i = index; i > 0; i--) {
        const Expr& item = At(p, Item(p, d, i - 1));
        if (item.kind != ExprKind::Let) {
            break;
        }
        if (item.ref == At(p, cond.a).ref) {
            return Item(p, d, i - 1);
        }
    }
    return before;
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
