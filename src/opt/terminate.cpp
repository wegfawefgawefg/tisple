// Which loops provably end. Every for does: its range is worked out once and the counter
// is set from it each pass (the body cannot set it). A while ends when its condition is a
// compare of a counter with a bound the loop never changes, and the counter's one write in
// the loop is a step towards the bound, run on every pass, that cannot wrap past it:
//
//   (while (< i 10) (set i (+ i 3)) ...)   ends: from any start, i passes 9 without wrapping
//   (while (< i 255) (set i (+ i 3)) ...)  kept: 254 + 3 wraps to 1
//   (while (!= i k) (set i (+ i 1)) ...)   ends: an odd step comes round to every value
//
// Anything else, a while true with a break among them, counts as possibly endless; deleting
// such a loop would change what the program does.

#include "opt/opt_internal.hpp"

namespace tisple::opt {

namespace {

// how many times code sets, lets or loops over var
std::uint32_t CountWrites(const Pass& p, ExprId id, std::int32_t var) {
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
    count += CountWrites(p, e.a, var) + CountWrites(p, e.b, var) + CountWrites(p, e.c, var);
    if (HasItems(e.kind)) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            count += CountWrites(p, Item(p, e, i), var);
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            count += CountWrites(p, p.program->clauses[e.first + i].body, var);
        }
    }
    return count;
}

std::int64_t ValueOf(TypeKind kind, std::uint32_t raw) {
    switch (kind) {
    case TypeKind::I8:
        return static_cast<std::int8_t>(raw & 0xFF);
    case TypeKind::I16:
        return static_cast<std::int16_t>(raw & 0xFFFF);
    case TypeKind::U8:
        return raw & 0xFF;
    default:
        return raw & 0xFFFF;
    }
}

// the step's signed amount when stmt is (set i (+ i c)), (set i (+ c i)) or (set i (- i c));
// 0 when it is not one. raw gets c's bits
std::int64_t StepOf(const Pass& p, ExprId stmt, std::int32_t var, std::uint32_t& raw) {
    const Expr& set = At(p, stmt);
    if (set.kind != ExprKind::Set || At(p, set.a).kind != ExprKind::VarRef ||
        At(p, set.a).ref != var) {
        return 0;
    }
    const Expr& op = At(p, set.b);
    if (op.kind != ExprKind::Binary || (op.op != Op::Add && op.op != Op::Sub) ||
        op.type != VarOf(p, var).type) {
        return 0;
    }
    ExprId other = -1;
    const Expr& a = At(p, op.a);
    const Expr& b = At(p, op.b);
    if (a.kind == ExprKind::VarRef && a.ref == var) {
        other = op.b;
    } else if (op.op == Op::Add && b.kind == ExprKind::VarRef && b.ref == var) {
        other = op.a;
    }
    if (other < 0 || At(p, other).kind != ExprKind::Literal) {
        return 0;
    }
    raw = At(p, other).value;
    std::int64_t c = ValueOf(KindOf(*p.program, op.type), raw);
    return op.op == Op::Add ? c : -c;
}

// a counter compared with a bound: does the loop end by it?
bool CounterEnds(const Pass& p, ExprId loop, ExprId counter, ExprId bound, Op op) {
    const Expr& w = At(p, loop);
    const Expr& ce = At(p, counter);
    if (ce.kind != ExprKind::VarRef) {
        return false;
    }
    std::int32_t var = ce.ref;
    const Var& v = VarOf(p, var);
    TypeKind kind = KindOf(*p.program, v.type);
    bool integer = kind == TypeKind::U8 || kind == TypeKind::I8 || kind == TypeKind::U16 ||
                   kind == TypeKind::I16;
    if ((v.kind != VarKind::Local && v.kind != VarKind::Param) || v.address_taken || !integer) {
        return false;
    }
    // the bound: a constant, or a local the loop never sets
    const Expr& be = At(p, bound);
    bool known = be.kind == ExprKind::Literal;
    std::int64_t k = known ? ValueOf(kind, be.value) : 0;
    if (!known) {
        if (be.kind != ExprKind::VarRef || be.ref == var) {
            return false;
        }
        const Var& bv = VarOf(p, be.ref);
        if ((bv.kind != VarKind::Local && bv.kind != VarKind::Param) || bv.address_taken ||
            CountWrites(p, loop, be.ref) != 0) {
            return false;
        }
    }
    // one write of the counter in the loop: a step, an item of the body, that every pass
    // reaches (no continue before it)
    if (CountWrites(p, loop, var) != 1) {
        return false;
    }
    const Expr& body = At(p, w.b);
    if (body.kind != ExprKind::Do) {
        return false;
    }
    std::int64_t step = 0;
    std::uint32_t raw = 0;
    for (std::uint32_t i = 0; i < body.count && step == 0; i++) {
        ExprId item = Item(p, body, i);
        step = StepOf(p, item, var, raw);
        if (step == 0 && ContinuesHere(p, item, 0)) {
            return false;
        }
    }
    if (step == 0) {
        return false;
    }
    std::int64_t bits = kind == TypeKind::U8 || kind == TypeKind::I8 ? 8 : 16;
    bool is_signed = kind == TypeKind::I8 || kind == TypeKind::I16;
    std::int64_t min = is_signed ? -(std::int64_t(1) << (bits - 1)) : 0;
    std::int64_t max =
        is_signed ? (std::int64_t(1) << (bits - 1)) - 1 : (std::int64_t(1) << bits) - 1;
    switch (op) {
    case Op::Lt:
        return step > 0 && (known ? k - 1 + step <= max : step == 1);
    case Op::Le:
        return step > 0 && known && k + step <= max;
    case Op::Gt:
        return step < 0 && (known ? k + 1 + step >= min : step == -1);
    case Op::Ge:
        return step < 0 && known && k + step >= min;
    case Op::Ne:
        return (raw & 1) != 0;
    default:
        return false;
    }
}

Op Flipped(Op op) {
    switch (op) {
    case Op::Lt:
        return Op::Gt;
    case Op::Le:
        return Op::Ge;
    case Op::Gt:
        return Op::Lt;
    case Op::Ge:
        return Op::Le;
    default:
        return op;
    }
}

// a condition that is false once some counter reaches its bound
bool CondEnds(const Pass& p, ExprId loop, ExprId cond) {
    const Expr& c = At(p, cond);
    if (c.kind == ExprKind::And) {
        return CondEnds(p, loop, c.a) || CondEnds(p, loop, c.b);
    }
    bool compare =
        c.op == Op::Lt || c.op == Op::Le || c.op == Op::Gt || c.op == Op::Ge || c.op == Op::Ne;
    if (c.kind != ExprKind::Binary || !compare) {
        return false;
    }
    return CounterEnds(p, loop, c.a, c.b, c.op) || CounterEnds(p, loop, c.b, c.a, Flipped(c.op));
}

} // namespace

// a continue inside id that goes to the loop id sits in (depth 0), not to a loop inside it
bool ContinuesHere(const Pass& p, ExprId id, int depth) {
    if (id < 0) {
        return false;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Continue:
        return depth == 0;
    case ExprKind::While:
        return ContinuesHere(p, e.a, depth) || ContinuesHere(p, e.b, depth + 1);
    case ExprKind::For:
        return ContinuesHere(p, e.a, depth) || ContinuesHere(p, e.b, depth) ||
               ContinuesHere(p, e.c, depth + 1);
    default:
        break;
    }
    if (ContinuesHere(p, e.a, depth) || ContinuesHere(p, e.b, depth) ||
        ContinuesHere(p, e.c, depth)) {
        return true;
    }
    if (HasItems(e.kind)) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (ContinuesHere(p, Item(p, e, i), depth)) {
                return true;
            }
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (ContinuesHere(p, p.program->clauses[e.first + i].body, depth)) {
                return true;
            }
        }
    }
    return false;
}

bool WhileEnds(const Pass& p, ExprId loop) {
    return CondEnds(p, loop, At(p, loop).a);
}

bool CodeTerminates(const Pass& p, ExprId id) {
    if (id < 0) {
        return true;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::While:
        if (!WhileEnds(p, id)) {
            return false;
        }
        break;
    case ExprKind::Call:
        if (!p.summaries[static_cast<std::size_t>(e.ref)].terminates) {
            return false;
        }
        break;
    case ExprKind::CallPtr: {
        // every function it may reach (callees, so their summaries are done) ends
        const std::vector<std::int32_t>& targets = PointerTargets(p, At(p, e.a).type);
        for (std::int32_t target : targets) {
            if (!p.summaries[static_cast<std::size_t>(target)].terminates) {
                return false;
            }
        }
        if (targets.empty()) {
            return false;
        }
        break;
    }
    case ExprKind::Literal:
    case ExprKind::FnRef:
    case ExprKind::VarRef:
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Load:
    case ExprKind::Store:
    case ExprKind::LoadField:
    case ExprKind::StoreField:
    case ExprKind::AddrOf:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::And:
    case ExprKind::Or:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
    case ExprKind::Interrupts:
    case ExprKind::AuxSwitch:
    case ExprKind::StructLit:
    case ExprKind::If:
    case ExprKind::Do:
    case ExprKind::For:
    case ExprKind::Match:
    case ExprKind::Return:
    case ExprKind::Break:
    case ExprKind::Continue:
    case ExprKind::Let:
    case ExprKind::Set:
        break;
    default:
        // machine code, and anything not known here, may never come back
        return false;
    }
    if (!CodeTerminates(p, e.a) || !CodeTerminates(p, e.b) || !CodeTerminates(p, e.c)) {
        return false;
    }
    if (HasItems(e.kind)) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (!CodeTerminates(p, Item(p, e, i))) {
                return false;
            }
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (!CodeTerminates(p, p.program->clauses[e.first + i].body)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace tisple::opt
