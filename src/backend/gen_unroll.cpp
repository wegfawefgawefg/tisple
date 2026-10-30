#include "backend/gen.hpp"
#include "check/arith.hpp"

// Unrolled for loops. One whose counter nothing reads (f.down) runs as copies of its body. A
// for loop with constant bounds and a few trips whose body reads the counter: copies of the
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
    if (!ConstValue(program, e.a, cs) || (e.b >= 0 && !ConstValue(program, e.b, ce))) {
        return 0;
    }
    TypeId type = program.vars[static_cast<std::size_t>(e.ref)].type;
    TypeKind kind = KindOf(program, type);
    // no end: through the type's largest value
    std::int32_t past = TypeOf(program, type).size == 1 ? 0x100 : 0x10000;
    std::int32_t end = e.b >= 0 ? SignedValue(kind, ce) : IsSignedKind(kind) ? past / 2 : past;
    std::int32_t trips = end - SignedValue(kind, cs);
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

namespace {

// copies of the body, one per trip; counted: each binds the counter to its literal
void MakeCopies(Gen& g, const ForParts& f, bool counted) {
    const Program& program = *g.program;
    std::uint32_t trips = f.trips == 0 ? 256 : f.trips;
    ExprId first = -1;
    if (counted) {
        trips = static_cast<std::uint32_t>(ConstTrips(program, ExprOf(program, f.id)));
        first = g.be->counter_literals[static_cast<std::size_t>(f.id)];
    }
    std::uint32_t around = g.unrolled_copies;
    g.unrolled_copies = around * trips;
    // each copy runs once
    g.loop_factors.back() = 1;
    g.loop_runs.back() = 1;
    for (std::uint32_t k = 0; k < trips; k++) {
        std::int32_t next = NewLabel(g);
        g.loops.back().continue_label = next;
        if (counted) {
            g.bound.push_back({f.var, first + static_cast<ExprId>(k)});
        }
        GenStmt(g, f.body);
        if (counted) {
            g.bound.pop_back();
        }
        PlaceLabel(g, next);
    }
    g.unrolled_copies = around;
}

std::vector<std::uint64_t> SymbolWeights(const Backend& be) {
    std::vector<std::uint64_t> out(be.info.size());
    for (std::size_t s = 0; s < be.info.size(); s++) {
        out[s] = be.info[s].weight;
    }
    return out;
}

// the frame bytes the function's code took so far: what a way of making the loop not kept
// takes is not kept either
struct FrameTops {
    std::uint32_t temps = 0;
    std::uint32_t ptrs = 0;
    std::uint32_t loop_ptrs = 0;
};

FrameTops GetTops(const Gen& g) {
    std::size_t f = static_cast<std::size_t>(g.function);
    return FrameTops{g.be->temp_size[f], g.be->ptemp_size[f], g.be->lptr_size[f]};
}

void SetTops(Gen& g, const FrameTops& tops) {
    std::size_t f = static_cast<std::size_t>(g.function);
    g.be->temp_size[f] = tops.temps;
    g.be->ptemp_size[f] = tops.ptrs;
    g.be->lptr_size[f] = tops.loop_ptrs;
}

// the symbols' weights as they were (symbols made since weigh nothing)
void SetWeights(Backend& be, const std::vector<std::uint64_t>& weights) {
    for (std::size_t s = 0; s < be.info.size(); s++) {
        be.info[s].weight = s < weights.size() ? weights[s] : 0;
    }
}

} // namespace

bool CountedCopiesFit(const Gen& g, const ForParts& f) {
    const Program& program = *g.program;
    std::size_t id = static_cast<std::size_t>(f.id);
    if (id >= g.be->counter_literals.size() || g.be->counter_literals[id] < 0) {
        return false;
    }
    std::uint32_t trips = static_cast<std::uint32_t>(ConstTrips(program, ExprOf(program, f.id)));
    // copies of a call that is not inlined everywhere are calls, the body's one copy lost
    return CountExprs(program, f.body) * trips * g.unrolled_copies <= kMaxCountedExprs &&
           !HasBigCalls(program, f.body, g.be->inlines);
}

void CopiesOrLoop(Gen& g, const ForParts& f, bool counted) {
    if (KeepsLoop(g, f.id)) {
        RolledLoop(g, f);
        return;
    }
    // the copies
    std::size_t mark = g.out->size();
    Gen start = g;
    // the weight of the code around the loop (inside it, the copies run once each)
    g.loop_depth--;
    std::uint64_t weight = LoopWeight(g);
    std::uint64_t runs = RunWeight(g);
    g.loop_depth++;
    std::vector<std::uint64_t> weights = SymbolWeights(*g.be);
    FrameTops tops = GetTops(g);
    g.open_notes.push_back({true, f.id});
    MakeCopies(g, f, counted);
    g.open_notes.pop_back();
    Gen copies = g;
    std::vector<AsmItem> copy_items(g.out->begin() + static_cast<std::ptrdiff_t>(mark),
                                    g.out->end());
    std::vector<std::uint64_t> copy_weights = SymbolWeights(*g.be);
    FrameTops copy_tops = GetTops(g);
    std::int64_t copy_bytes = RawBytes(g, mark);
    std::uint64_t copy_cost = g.cost + g.routine_cost - start.cost - start.routine_cost;
    if (g.failed) {
        return;
    }
    // the loop
    g = start;
    g.out->resize(mark);
    SetWeights(*g.be, weights);
    SetTops(g, tops);
    RolledLoop(g, f);
    std::int64_t loop_bytes = RawBytes(g, mark);
    std::uint64_t loop_cost = g.cost + g.routine_cost - start.cost - start.routine_cost;
    // the copies look bigger: after the clean-up they may not be (stores that later copies
    // write again go, the loop's never)
    if (copy_bytes > loop_bytes) {
        copy_bytes = CleanBytes(g, copy_items);
        loop_bytes =
            CleanBytes(g, std::vector<AsmItem>(g.out->begin() + static_cast<std::ptrdiff_t>(mark),
                                               g.out->end()));
    }
    // no faster and no smaller: the loop
    if (g.failed || (copy_cost >= loop_cost && copy_bytes >= loop_bytes)) {
        return;
    }
    g = copies;
    g.out->resize(mark);
    g.out->insert(g.out->end(), copy_items.begin(), copy_items.end());
    SetWeights(*g.be, copy_weights);
    SetTops(g, copy_tops);
    SizeNote note;
    note.item = {true, f.id};
    note.site = f.id;
    note.function = g.function;
    note.bytes = copy_bytes;
    note.instead = loop_bytes;
    note.saved = static_cast<std::int64_t>(loop_cost) - static_cast<std::int64_t>(copy_cost);
    note.weight = weight;
    note.runs = runs;
    note.around = g.open_notes;
    g.size_notes.push_back(note);
}

} // namespace tisple
