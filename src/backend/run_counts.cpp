#include "backend/analysis.hpp"
#include "backend/frames.hpp"
#include "check/arith.hpp"

#include <algorithm>

// How often code runs, estimated before code generation: a loop's body by its trip count (a
// for with constant bounds; else 8 for a while, 4 for a for), a function by the calls to it,
// each weighed by the loops around it and by how often its caller runs. main runs once; with
// no main every function that nothing calls does. The interrupt handler runs over and over
// while main does, as often as a while loop's body (kHandlerRuns).

namespace tisple {

namespace {

constexpr std::uint64_t kMaxRuns = std::uint64_t(1) << 32;
constexpr std::uint64_t kHandlerRuns = 8; // LoopTrips' while

std::uint64_t Times(std::uint64_t a, std::uint64_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    return a > kMaxRuns / b ? kMaxRuns : a * b;
}

// the calls under id, each running `runs` times per run of the code at id
void AddCalls(const Program& program, ExprId id, std::uint64_t runs, std::uint32_t most,
              std::vector<std::uint64_t>& out) {
    if (id < 0) {
        return;
    }
    const Expr& e = ExprOf(program, id);
    if (e.kind == ExprKind::While || e.kind == ExprKind::For) {
        std::uint64_t body = Times(runs, LoopTrips(program, id, most));
        AddCalls(program, e.a, e.kind == ExprKind::While ? body : runs, most, out);
        AddCalls(program, e.b, e.kind == ExprKind::While ? body : runs, most, out);
        AddCalls(program, e.c, body, most, out);
        return;
    }
    if (e.kind == ExprKind::Call) {
        std::size_t callee = static_cast<std::size_t>(e.ref);
        out[callee] = std::min(kMaxRuns, out[callee] + runs);
    }
    AddCalls(program, e.a, runs, most, out);
    AddCalls(program, e.b, runs, most, out);
    AddCalls(program, e.c, runs, most, out);
    switch (e.kind) {
    case ExprKind::Call:
    case ExprKind::StructLit:
    case ExprKind::Do:
        for (std::uint32_t i = 0; i < e.count; i++) {
            AddCalls(program, ListItem(program, e, i), runs, most, out);
        }
        return;
    case ExprKind::Match:
        for (std::uint32_t i = 0; i < e.count; i++) {
            AddCalls(program, program.clauses[e.first + i].body, runs, most, out);
        }
        return;
    default:
        return;
    }
}

} // namespace

std::uint32_t LoopTrips(const Program& program, ExprId loop, std::uint32_t most) {
    const Expr& e = ExprOf(program, loop);
    if (e.kind == ExprKind::While) {
        return 8;
    }
    std::uint16_t start = 0;
    std::uint16_t end = 0;
    if (e.kind != ExprKind::For || !ConstValue(program, e.a, start)) {
        return 4;
    }
    TypeKind kind = KindOf(program, program.vars[static_cast<std::size_t>(e.ref)].type);
    std::int32_t from = SignedValue(kind, start);
    std::int32_t to = 0;
    if (e.b < 0) {
        std::uint32_t bits = KindBits(kind);
        to = IsSignedKind(kind) ? (1 << (bits - 1)) : (1 << bits);
    } else if (ConstValue(program, e.b, end)) {
        to = SignedValue(kind, end);
    } else {
        return 4;
    }
    std::int32_t trips = to - from;
    std::uint32_t n = static_cast<std::uint32_t>(trips < 2 ? 2 : trips);
    return n > most ? most : n;
}

std::vector<std::uint64_t> FindRunCounts(const Program& program, std::uint32_t most) {
    std::size_t count = program.functions.size();
    std::vector<std::uint64_t> runs(count, 0);
    std::vector<bool> called(count, false);
    for (const Function& f : program.functions) {
        for (std::int32_t callee : f.callees) {
            called[static_cast<std::size_t>(callee)] = true;
        }
    }
    for (std::size_t f = 0; f < count; f++) {
        bool root = program.main >= 0 ? static_cast<std::int32_t>(f) == program.main : !called[f];
        runs[f] = root ? 1 : 0;
        if (static_cast<std::int32_t>(f) == program.handler) {
            runs[f] = kHandlerRuns;
        }
    }
    for (std::int32_t f : CallersFirst(program)) {
        std::uint64_t r = runs[static_cast<std::size_t>(f)];
        if (r > 0) {
            AddCalls(program, program.functions[static_cast<std::size_t>(f)].body, r, most, runs);
        }
    }
    return runs;
}

} // namespace tisple
