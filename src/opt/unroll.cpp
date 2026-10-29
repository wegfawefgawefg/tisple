// Full unrolling of short loops (docs/midlevel-optimizer.md, slice 5): a loop collapse could
// not run once, whose passes depend on the ones before, becomes a copy of its body per pass,
// so constant propagation, ranges and dead code see each pass's values:
//   (for i i16 -3 4 (set g (| i g)))  ->  (do (set g (| -3 g))) ... (do (set g (| 3 g)))
// and (| x -1) is all ones from the third copy on. A for with a literal range puts each
// pass's counter in its copy; a counting while ((let c v) just before) keeps its step in
// each copy, and constant propagation follows c.
// Only loops of at most kMaxTrips passes whose copies come to at most kMaxNodes nodes, with no
// calls or hardware (see Keeps), that never leave a pass early (break, continue, return), and
// not under a shared node; and only once the other passes have settled (opt.cpp).

#include "check/arith.hpp"
#include "opt/collapse.hpp"
#include "opt/licm.hpp"

namespace tisple::opt {

namespace {

constexpr std::int64_t kMaxTrips = 8;
constexpr int kMaxNodes = 96;

// a call (copies of a call stop the backend inlining it, and its effects stop the folding the
// copies are for), and what hardware sees, whose timing a copy per pass would change: I/O,
// jsr, waits, :address, aux and :reserve variables
bool Keeps(const Pass& p, const Expr& e) {
    switch (e.kind) {
    case ExprKind::Call:
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
    case ExprKind::Jsr:
    case ExprKind::WaitCycles:
        return true;
    case ExprKind::VarRef:
        return Hardware(VarOf(p, e.ref));
    default:
        return false;
    }
}

// nodes under id, counted up to limit; what Keeps a loop counts as the limit
int CountNodes(const Pass& p, ExprId id, int limit) {
    if (id < 0 || limit <= 0) {
        return 0;
    }
    int n = 1;
    const Expr& e = At(p, id);
    if (Keeps(p, e)) {
        return limit + 1;
    }
    EachChild(p, e, [&](ExprId child) {
        if (n <= limit) {
            n += CountNodes(p, child, limit - n);
        }
    });
    return n;
}

// a copy of id, every read of var made the literal bits (var -1: none). The variables lets
// and fors in it declare get new locals of function (names: what each was renamed to), so no
// variable has two lets that may both hold (the backend binds a let's variable to its value);
// function -1: none are
ExprId Clone(Pass& p, ExprId id, std::int32_t var, std::uint32_t bits, std::int32_t function,
             std::unordered_map<std::int32_t, std::int32_t>& names) {
    Expr e = At(p, id);
    if (e.kind == ExprKind::VarRef && e.ref == var) {
        Expr lit;
        lit.kind = ExprKind::Literal;
        lit.type = e.type;
        lit.value = MaskToKind(KindOf(*p.program, e.type), bits);
        lit.loc = e.loc;
        return NewExpr(p, lit);
    }
    bool declares = e.kind == ExprKind::Let || e.kind == ExprKind::For;
    if (declares && function >= 0 && names.find(e.ref) == names.end()) {
        names[e.ref] = CopyLocal(p, function, e.ref);
    }
    auto renamed = names.find(e.ref);
    bool named = e.kind == ExprKind::VarRef || declares;
    if (named && renamed != names.end()) {
        e.ref = renamed->second;
    }
    for (ExprId* child : {&e.a, &e.b, &e.c}) {
        if (*child >= 0) {
            *child = Clone(p, *child, var, bits, function, names);
        }
    }
    if (e.kind == ExprKind::Call || e.kind == ExprKind::StructLit || e.kind == ExprKind::Do) {
        std::vector<ExprId> items;
        for (std::uint32_t i = 0; i < e.count; i++) {
            items.push_back(Clone(p, Item(p, e, i), var, bits, function, names));
        }
        e.first = static_cast<std::uint32_t>(p.program->lists.size());
        p.program->lists.insert(p.program->lists.end(), items.begin(), items.end());
    }
    if (e.kind == ExprKind::Match) {
        std::vector<Clause> clauses;
        for (std::uint32_t i = 0; i < e.count; i++) {
            Clause clause = p.program->clauses[e.first + i];
            clause.body = Clone(p, clause.body, var, bits, function, names);
            clauses.push_back(clause);
        }
        e.first = static_cast<std::uint32_t>(p.program->clauses.size());
        p.program->clauses.insert(p.program->clauses.end(), clauses.begin(), clauses.end());
    }
    return NewExpr(p, e);
}

// each copy a do of its own: its lets stay in its scope
ExprId Copy(Pass& p, std::int32_t function, ExprId body, std::int32_t var, std::uint32_t bits) {
    std::unordered_map<std::int32_t, std::int32_t> names;
    ExprId copy = Clone(p, body, var, bits, function, names);
    Expr e = At(p, copy); // a copy: NewList moves every expression
    if (e.kind == ExprKind::Do && e.type == kVoidType) {
        return copy;
    }
    return NewList(p, ExprKind::Do, kVoidType, {copy}, e.loc);
}

} // namespace

std::int32_t CopyLocal(Pass& p, std::int32_t function, std::int32_t var) {
    Var copy = VarOf(p, var);
    copy.name = "opt." + std::to_string(p.program->vars.size());
    copy.function = function;
    std::int32_t id = static_cast<std::int32_t>(p.program->vars.size());
    p.program->vars.push_back(copy);
    p.program->functions[static_cast<std::size_t>(function)].locals.push_back(id);
    return id;
}

ExprId CloneExpr(Pass& p, ExprId id, std::int32_t function,
                 std::unordered_map<std::int32_t, std::int32_t>& names) {
    return Clone(p, id, -1, 0, function, names);
}

ExprId CloneExpr(Pass& p, ExprId id) {
    std::unordered_map<std::int32_t, std::int32_t> names;
    return Clone(p, id, -1, 0, -1, names);
}

bool UnrollLoop(Pass& p, std::int32_t function, ExprId loop, ExprId before,
                std::vector<ExprId>& out) {
    Expr e = At(p, loop);
    bool is_for = e.kind == ExprKind::For;
    ExprId body = is_for ? e.c : e.b;
    std::int64_t trips = KnownTrips(p, loop, is_for ? -1 : before);
    if (trips < 2 || trips > kMaxTrips || MayLeave(p, body, 0)) {
        return false;
    }
    if (is_for && (VarOf(p, e.ref).address_taken || !IsPure(p, e.a) || !IsPure(p, e.b))) {
        return false;
    }
    if (trips * CountNodes(p, body, kMaxNodes + 1) > kMaxNodes ||
        !Allow(p, "loop unrolled", loop, is_for ? e.ref : -1)) {
        return false;
    }
    std::uint16_t start = 0;
    if (is_for) {
        ConstValue(*p.program, e.a, start);
    }
    for (std::int64_t k = 0; k < trips; k++) {
        out.push_back(
            Copy(p, function, body, is_for ? e.ref : -1, static_cast<std::uint32_t>(start + k)));
    }
    return true;
}

} // namespace tisple::opt
