#include "backend/gen.hpp"
#include "check/arith.hpp"

// for loops with constant bounds and a few trips whose body reads the counter: copies of the
// body, the counter in each a constant. Each copy binds the counter to a literal of its value
// (Gen::bound, as an inlined call binds a parameter to its argument), so a read of it is that
// number and (at a i) a fixed address; the counter's memory is never set. The literals are
// made before code generation (AddCounterLiterals), since the generator only reads the
// program.

namespace tisple {

namespace {

// trips, and expressions in all the copies together (with the copies of unrolled loops
// around them), as for loops whose counter nothing reads (gen_loop.cpp)
constexpr std::int32_t kMaxCountedTrips = 16;
constexpr std::uint32_t kMaxCountedExprs = 160;

// the trips of a for with constant bounds, or 0
std::int32_t ConstTrips(const Program& program, const Expr& e) {
    std::uint16_t cs = 0;
    std::uint16_t ce = 0;
    if (e.b < 0 || !ConstValue(program, e.a, cs) || !ConstValue(program, e.b, ce)) {
        return 0;
    }
    TypeKind kind = KindOf(program, program.vars[static_cast<std::size_t>(e.ref)].type);
    std::int32_t trips = SignedValue(kind, ce) - SignedValue(kind, cs);
    return trips > 0 ? trips : 0;
}

} // namespace

std::vector<ExprId> AddCounterLiterals(Program& program) {
    std::size_t count = program.exprs.size();
    std::vector<ExprId> out(count, -1);
    for (std::size_t i = 0; i < count; i++) {
        Expr e = program.exprs[i];
        if (e.kind != ExprKind::For) {
            continue;
        }
        std::int32_t trips = ConstTrips(program, e);
        if (trips < 2 || trips > kMaxCountedTrips || !ReadsVar(program, e.c, e.ref)) {
            continue;
        }
        TypeId type = program.vars[static_cast<std::size_t>(e.ref)].type;
        std::uint32_t mask = TypeOf(program, type).size == 2 ? 0xFFFF : 0xFF;
        std::uint16_t start = 0;
        ConstValue(program, e.a, start);
        out[i] = static_cast<ExprId>(program.exprs.size());
        for (std::int32_t k = 0; k < trips; k++) {
            Expr literal;
            literal.kind = ExprKind::Literal;
            literal.type = type;
            literal.value = (start + static_cast<std::uint32_t>(k)) & mask;
            literal.loc = e.loc;
            program.exprs.push_back(literal);
        }
    }
    return out;
}

bool CountedCopies(Gen& g, const ForParts& f) {
    const Program& program = *g.program;
    std::size_t id = static_cast<std::size_t>(f.id);
    if (id >= g.be->counter_literals.size() || g.be->counter_literals[id] < 0) {
        return false;
    }
    std::uint32_t trips = static_cast<std::uint32_t>(ConstTrips(program, ExprOf(program, f.id)));
    // copies of a call that is not inlined everywhere are calls, the body's one copy lost
    if (CountExprs(program, f.body) * trips * g.unrolled_copies > kMaxCountedExprs ||
        HasBigCalls(program, f.body, g.be->inlines)) {
        return false;
    }
    ExprId first = g.be->counter_literals[id];
    std::uint32_t around = g.unrolled_copies;
    g.unrolled_copies = around * trips;
    // each copy runs once
    g.loop_factors.back() = 1;
    for (std::uint32_t k = 0; k < trips; k++) {
        std::int32_t next = NewLabel(g);
        g.loops.back().continue_label = next;
        g.bound.push_back({f.var, first + static_cast<ExprId>(k)});
        GenStmt(g, f.body);
        g.bound.pop_back();
        PlaceLabel(g, next);
    }
    g.unrolled_copies = around;
    return true;
}

} // namespace tisple
