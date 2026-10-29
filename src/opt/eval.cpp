// Compile-time evaluation (docs/midlevel-optimizer.md, pass 7): a call of a pure function
// with every argument constant runs in the reference interpreter now, and the call becomes
// the value it returned.
//
// - Pure here: its summary (callees included) writes no global and nothing through a pointer,
//   does no I/O or jsr and has no node the summaries do not know (wait_cycles among them), and
//   every loop in it provably ends; it reads no global either (whose value now is not the
//   value then) and nothing through a pointer. Const arrays it may read: their bytes are fixed.
// - Integers, bools and enums returned: what a literal can hold.
// - A step budget (one step per expression the interpreter works out) keeps compile time
//   small: kCallSteps for one call, kTotalSteps for every call of one compile. A call over
//   its budget stays a call, and is not tried again.
// - Inner calls first, so (f (g 3)) is worked out whole in one round. A call tried once is
//   remembered with its value for the rest of the compile.
// - The interpreter runs the function as it is now (the passes keep what it computes) on a
//   layout of the program made with LayoutProgram's defaults; a program whose data do not fit
//   there is left as it is.

#include "check/arith.hpp"
#include "interp/interp.hpp"
#include "link/layout.hpp"
#include "opt/consts.hpp"

#include <algorithm>

namespace tisple::opt {

namespace {

constexpr std::uint64_t kCallSteps = 20000;
constexpr std::uint64_t kTotalSteps = 400000;

struct Evaluator {
    Pass* p = nullptr;
    bool laid_out = false; // the layout was tried
    bool ready = false;    // and the interpreter runs on it
    Layout layout;
    Interp in;
    std::uint64_t changes = 0;
};

// a function the interpreter can run now, giving what the 6502 would give then
bool RunsNow(const Pass& p, std::int32_t function) {
    if (!PureFunction(p, function)) {
        return false;
    }
    const Summary& s = p.summaries[static_cast<std::size_t>(function)];
    if (s.loads) {
        return false;
    }
    for (bool read : s.reads) {
        if (read) {
            return false;
        }
    }
    TypeId ret = p.program->functions[static_cast<std::size_t>(function)].ret;
    return ConstType(*p.program, ret);
}

bool Ready(Evaluator& ev) {
    if (ev.laid_out) {
        return ev.ready;
    }
    ev.laid_out = true;
    std::vector<Diagnostic> errors;
    if (!LayoutProgram(*ev.p->program, LayoutOptions(), ev.layout, errors) || ev.layout.no_room) {
        return false;
    }
    InitInterp(ev.in, *ev.p->program, ev.layout);
    ev.ready = true;
    return true;
}

// the value of (function args...), or -1 when it cannot be worked out within the budget
std::int32_t Run(Evaluator& ev, std::int32_t function, const std::vector<std::uint16_t>& args) {
    Pass& p = *ev.p;
    std::vector<std::uint32_t> key = {static_cast<std::uint32_t>(function)};
    key.insert(key.end(), args.begin(), args.end());
    auto found = p.evaluated.find(key);
    if (found != p.evaluated.end()) {
        return found->second;
    }
    std::int32_t value = -1;
    if (p.eval_steps < kTotalSteps && Ready(ev)) {
        ev.in.max_steps = std::min(kCallSteps, kTotalSteps - p.eval_steps);
        InterpResult result = CallFunction(ev.in, function, args);
        p.eval_steps += result.steps;
        if (result.kind == StopKind::Finished && result.has_value) {
            TypeKind kind = KindOf(*p.program, result.type);
            value = MaskToKind(kind, result.value);
        }
    }
    p.evaluated[key] = value;
    return value;
}

// a call with constant arguments to a function that runs now becomes its value
void TryCall(Evaluator& ev, ExprId id) {
    Pass& p = *ev.p;
    const Expr& e = At(p, id);
    if (!RunsNow(p, e.ref)) {
        return;
    }
    std::vector<std::uint16_t> args;
    for (std::uint32_t i = 0; i < e.count; i++) {
        std::uint16_t bits = 0;
        if (!ConstValue(*p.program, Item(p, e, i), bits)) {
            return;
        }
        args.push_back(bits);
    }
    std::int32_t value = Run(ev, e.ref, args);
    if (value < 0 || !Allow(p, "call worked out", id, -1)) {
        return;
    }
    ReplaceWithLiteral(p, id, static_cast<std::uint16_t>(value));
    ev.changes++;
}

void Visit(Evaluator& ev, ExprId id, bool shared) {
    if (id < 0) {
        return;
    }
    Pass& p = *ev.p;
    shared = shared || Shared(p, id);
    Expr e = At(p, id);
    EachChild(p, e, [&](ExprId child) { Visit(ev, child, shared); });
    if (!shared && e.kind == ExprKind::Call) {
        TryCall(ev, id);
    }
}

} // namespace

std::uint64_t EvaluateCalls(Pass& p) {
    Evaluator ev;
    ev.p = &p;
    for (const Function& fn : p.program->functions) {
        Visit(ev, fn.body, false);
    }
    return ev.changes;
}

} // namespace tisple::opt
