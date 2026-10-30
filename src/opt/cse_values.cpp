// What common subexpressions (cse_walk.cpp) take as a value: integer or enum arithmetic and
// calls to pure functions that do not load through pointers, over plain variables (no
// :address, aux or :reserve, where hardware may answer differently each time), worth enough
// cycles to keep (licm_cost.cpp's estimate); and when two are the same.

#include "opt/cse.hpp"
#include "opt/licm.hpp"

namespace tisple::opt {

namespace {

// cycles a value must save, once kept in a local instead of worked out again
constexpr std::int32_t kMinSaving = 12;

bool PlainVar(const Var& v) {
    return !v.placement.has_address && !v.placement.in_aux && !v.placement.reserve && !v.irq_shared;
}

bool Stable(const Pass& p, ExprId id, CseEntry& e);

void Reads(const Pass& p, std::int32_t var, CseEntry& e) {
    e.reads.push_back(var);
    e.pointed = e.pointed || VarOf(p, var).address_taken;
}

// a place read as a value: rooted at a plain variable, its indexes stable
bool StablePlace(const Pass& p, ExprId id, CseEntry& e) {
    const Expr& x = At(p, id);
    switch (x.kind) {
    case ExprKind::VarRef:
        if (!PlainVar(VarOf(p, x.ref))) {
            return false;
        }
        Reads(p, x.ref, e);
        return true;
    case ExprKind::Index:
        return StablePlace(p, x.a, e) && Stable(p, x.b, e);
    case ExprKind::Field:
        return StablePlace(p, x.a, e);
    default:
        return false; // through a pointer
    }
}

bool StableCall(const Pass& p, const Expr& x, CseEntry& e) {
    if (!PureFunction(p, x.ref)) {
        return false;
    }
    const Summary& s = p.summaries[static_cast<std::size_t>(x.ref)];
    if (s.loads) {
        return false;
    }
    for (std::size_t v = 0; v < s.reads.size(); v++) {
        if (!s.reads[v]) {
            continue;
        }
        if (!PlainVar(VarOf(p, static_cast<std::int32_t>(v)))) {
            return false;
        }
        Reads(p, static_cast<std::int32_t>(v), e);
    }
    for (std::uint32_t i = 0; i < x.count; i++) {
        if (!Stable(p, Item(p, x, i), e)) {
            return false;
        }
    }
    return true;
}

// worked out twice with nothing changed in between, it gives the same value and does nothing
// else; e gets the variables it reads
bool Stable(const Pass& p, ExprId id, CseEntry& e) {
    if (id < 0) {
        return true;
    }
    const Expr& x = At(p, id);
    switch (x.kind) {
    case ExprKind::Literal:
        return true;
    case ExprKind::VarRef:
        if (!IsScalarKind(KindOf(*p.program, x.type))) {
            return false;
        }
        return StablePlace(p, id, e);
    case ExprKind::Index:
        return StablePlace(p, x.a, e) && Stable(p, x.b, e);
    case ExprKind::Field:
        return StablePlace(p, x.a, e);
    case ExprKind::Unary:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
        return Stable(p, x.a, e);
    case ExprKind::Binary:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
        return Stable(p, x.a, e) && Stable(p, x.b, e);
    case ExprKind::Call:
        return StableCall(p, x, e);
    default:
        return false;
    }
}

bool Wide(const Pass& p, const Expr& x) {
    return TypeOf(*p.program, x.type).size > 1;
}

// worth keeping in a local when it is worked out again once
bool Candidate(const Pass& p, ExprId id) {
    const Expr& x = At(p, id);
    TypeKind kind = KindOf(*p.program, x.type);
    if (!IsIntegerKind(kind) && kind != TypeKind::Enum) {
        return false; // a bool a branch tests directly, pointers, structs
    }
    switch (x.kind) {
    case ExprKind::Binary:
        // a widened byte times a constant: the backend works it out in A and one byte, but
        // into a local's two bytes with shifts of memory (the life kernel's row addresses,
        // once the row function is inlined): worked out twice it costs no more
        if (x.op == Op::Mul && Wide(p, x) && At(p, x.a).kind == ExprKind::Convert &&
            !Wide(p, At(p, At(p, x.a).a)) && At(p, x.b).kind == ExprKind::Literal) {
            return false;
        }
        break;
    case ExprKind::Unary:
    case ExprKind::Call:
        break;
    case ExprKind::Index:
        // an element read again: the backend reaches it again cheaply (an index register, a
        // pointer it keeps), and knows (set (at a i) (+ (at a i) x)) as one update in place
        return false;
    case ExprKind::Convert:
        // a byte widened: the backend knows its high byte is zero, which a local would hide
        if (Wide(p, x) && !Wide(p, At(p, x.a))) {
            return false;
        }
        break;
    default:
        return false;
    }
    // kept: worked out once, stored, read back twice
    std::int32_t word = Wide(p, x) ? 2 : 1;
    return EstimatedCycles(p, id) - 9 * word >= kMinSaving;
}

// the same value, written the same way (calls included)
bool Same(const Pass& p, ExprId a, ExprId b) {
    if (a == b) {
        return true;
    }
    if (a < 0 || b < 0) {
        return false;
    }
    const Expr& x = At(p, a);
    const Expr& y = At(p, b);
    if (x.kind != y.kind || x.type != y.type || x.op != y.op || x.ref != y.ref ||
        x.value != y.value || x.count != y.count) {
        return false;
    }
    switch (x.kind) {
    case ExprKind::Literal:
    case ExprKind::VarRef:
        return true;
    case ExprKind::Index:
    case ExprKind::Binary:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
        return Same(p, x.a, y.a) && Same(p, x.b, y.b);
    case ExprKind::Field:
    case ExprKind::Unary:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
        return Same(p, x.a, y.a);
    case ExprKind::Call:
        for (std::uint32_t i = 0; i < x.count; i++) {
            if (!Same(p, Item(p, x, i), Item(p, y, i))) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

} // namespace

bool CseValue(const Pass& p, ExprId id, CseEntry& e) {
    return Candidate(p, id) && Stable(p, id, e);
}

bool SameValue(const Pass& p, ExprId a, ExprId b) {
    return Same(p, a, b);
}

bool DivisionPartners(const Pass& p, ExprId a, ExprId b) {
    const Expr& x = At(p, a);
    const Expr& y = At(p, b);
    bool division = x.kind == ExprKind::Binary && y.kind == ExprKind::Binary &&
                    (x.op == Op::Div || x.op == Op::Mod) && (y.op == Op::Div || y.op == Op::Mod);
    return division && x.op != y.op && x.type == y.type && Same(p, x.a, y.a) && Same(p, x.b, y.b);
}

} // namespace tisple::opt
