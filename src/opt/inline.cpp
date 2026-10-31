// Inlining (docs/midlevel-optimizer.md, slice 6): a call becomes a copy of its function's body
// where it is made, so the other passes see what it does there: the globals a caller just set
// are constants in the copy (1134), a store the callee overwrites is dead (608), a loop whose
// passes do the same through a call collapses.
// - When: once a round changes nothing (opt.cpp), so each function is as small as the passes
//   make it, as the backend sees it.
// - Which calls: those the backend inlines (FindInlines, backend/analysis.cpp), so the code
//   stays about the size it was: every call of a function of at most kMaxInlineExprs nodes,
//   and the one call of a function called in one place, in a loop too (the backend keeps one
//   with loops of its own a JSR there; copied, the passes see through it: the line kernel
//   507296 to 506033 cycles). Not a small function given a for's counter: the backend copies
//   the counter (a register) to the parameter's byte, a better operand than the register (the
//   life kernel's rows). Never main, what --no-inline names (the race's clock ends), a
//   function whose address is taken, or the interrupt handler's code; only with a main (with
//   none, any function may be called from outside). Calls count in the functions that run now
//   (Live): one whose calls were all copied runs no more.
// - A call that is a statement gives its items to the do around it, as does (set x (f ...)) of
//   a plain variable, x set where each way through the body ends; any other call becomes a do
//   in its place (inline_copy.cpp makes the copies). A call in a function copied in this round
//   waits for the next, when the copies count as its calls (as the backend counts them).
// - A function no code that runs calls gives up its body, unless the build keeps every function
//   (tests call them by address); the interpreter runs the program as written, and the frames
//   keep their order (Function::callees is left as it is).

#include "opt/inline.hpp"

#include "backend/analysis.hpp"
#include "check/aux_mode.hpp"
#include "opt/collapse.hpp"

#include <algorithm>

namespace tisple::opt {

namespace {

// nodes copied in one compile at most (the compile stays fast)
constexpr std::uint32_t kMaxCopied = 30000;
// steps down the call tree at each settled round
constexpr int kMaxSteps = 8;

struct Inliner {
    Pass* p = nullptr;
    std::int32_t function = -1; // being walked
    std::vector<bool> copied;   // by function: its calls are copied in this round
    std::vector<bool> waits;    // by function: its body is copied in this round
    std::vector<bool> once;     // by function: called in one place, not small
    std::vector<bool> as_value; // by function: copied where its value is used too
    bool dry = false;           // only count the calls that would be copied, into wanted
    std::vector<std::uint32_t> wanted;
    std::uint32_t budget = 0; // nodes left to copy
    std::vector<std::uint32_t> size;
    std::uint64_t changes = 0;
};

// an argument is a for's counter
bool CounterArgument(const Pass& p, const Expr& call) {
    for (std::uint32_t i = 0; i < call.count; i++) {
        const Expr& arg = At(p, Item(p, call, i));
        if (arg.kind == ExprKind::VarRef && VarOf(p, arg.ref).loop_var) {
            return true;
        }
    }
    return false;
}

// may this call be copied here
bool Wanted(Inliner& in, ExprId call, bool shared, bool statement) {
    Pass& p = *in.p;
    const Expr& e = At(p, call);
    std::size_t f = static_cast<std::size_t>(e.ref);
    if (e.kind != ExprKind::Call || shared || Shared(p, call) || !in.copied[f] ||
        in.waits[static_cast<std::size_t>(in.function)] || in.size[f] > in.budget ||
        (!statement && !in.as_value[f])) {
        return false;
    }
    // a small function given a for's counter: the backend copies the counter (a register) to
    // the parameter's byte, a better operand than the register the copy would read in place
    // (the life kernel's rows)
    if (!in.once[f] && CounterArgument(p, e)) {
        return false;
    }
    if (in.dry) {
        in.wanted[f]++;
        return false;
    }
    if (!Allow(p, "call inlined", call, -1)) {
        return false;
    }
    in.budget -= in.size[f];
    in.changes++;
    return true;
}

void Visit(Inliner& in, ExprId id, bool shared);

// a call anywhere but a do's item: a do in its place
void InPlace(Inliner& in, ExprId call) {
    Pass& p = *in.p;
    std::vector<ExprId> items;
    CopyCall(p, in.function, call, CopyMode::Value, -1, items);
    // named once (CopyCall's): the call takes its content, then it gives up its own
    ExprId made = items.size() == 1 ? items[0] : -1;
    if (made < 0) {
        made = NewList(p, ExprKind::Do, At(p, call).type, items, At(p, call).loc);
        for (ExprId item : items) {
            Release(p, item); // the new do names them
        }
        Retain(p, made);
    }
    ReplaceWith(p, call, made);
    Release(p, made);
}

// a do's items: statement calls and (set x (f ...)) give it the copy's items
void VisitDo(Inliner& in, ExprId id, bool shared) {
    Pass& p = *in.p;
    std::uint32_t i = 0;
    while (i < At(p, id).count) {
        ExprId item = Item(p, At(p, id), i);
        Expr e = At(p, item);
        bool value = i + 1 == At(p, id).count && At(p, id).type != kVoidType;
        CopyMode mode = CopyMode::Statement;
        ExprId call = item;
        std::int32_t dest = -1;
        if (e.kind == ExprKind::Set && At(p, e.a).kind == ExprKind::VarRef && !Shared(p, e.a) &&
            !Hardware(VarOf(p, At(p, e.a).ref))) {
            mode = CopyMode::SetVar;
            call = e.b;
            dest = At(p, e.a).ref;
        }
        if (value || shared || Shared(p, item) || At(p, call).kind != ExprKind::Call ||
            !Wanted(in, call, shared, mode == CopyMode::Statement)) {
            Visit(in, item, shared);
            i++;
            continue;
        }
        std::vector<ExprId> stmts;
        CopyCall(p, in.function, call, mode, dest, stmts);
        Expr d = At(p, id);
        std::vector<ExprId> items(p.program->lists.begin() + d.first,
                                  p.program->lists.begin() + d.first + d.count);
        items.erase(items.begin() + i);
        items.insert(items.begin() + i, stmts.begin(), stmts.end());
        Release(p, item); // the arguments live on in the lets
        Expr& out = p.program->exprs[static_cast<std::size_t>(id)];
        out.first = static_cast<std::uint32_t>(p.program->lists.size());
        out.count = static_cast<std::uint32_t>(items.size());
        p.program->lists.insert(p.program->lists.end(), items.begin(), items.end());
        // the copy waits for the next round
        i += static_cast<std::uint32_t>(stmts.size());
    }
}

void Visit(Inliner& in, ExprId id, bool shared) {
    if (id < 0) {
        return;
    }
    Pass& p = *in.p;
    shared = shared || Shared(p, id);
    Expr e = At(p, id);
    if (e.kind == ExprKind::Do) {
        VisitDo(in, id, shared);
        return;
    }
    if (e.kind == ExprKind::Call && Wanted(in, id, shared, false)) {
        InPlace(in, id);
        return;
    }
    EachChild(p, e, [&](ExprId child) { Visit(in, child, shared); });
}

void CountCalls(const Pass& p, ExprId id, std::vector<std::uint32_t>& calls) {
    if (id < 0) {
        return;
    }
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::Call) {
        calls[static_cast<std::size_t>(e.ref)]++;
    }
    EachChild(p, e, [&](ExprId child) { CountCalls(p, child, calls); });
}

// by function: does it run now? From main, the interrupt handler and every function whose
// address is taken (called from wherever its pointer goes), through the calls in the bodies
// as they are now: Function::callees still lists the calls copied away. Calls through pointers
// reach only address-taken functions, roots already
std::vector<bool> Live(const Pass& p) {
    const Program& program = *p.program;
    std::vector<bool> live(program.functions.size(), false);
    std::vector<std::int32_t> work;
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        const Function& fn = program.functions[f];
        std::int32_t id = static_cast<std::int32_t>(f);
        if (id == program.main || id == program.handler || fn.address_taken) {
            live[f] = true;
            work.push_back(id);
        }
    }
    while (!work.empty()) {
        std::vector<std::uint32_t> calls(program.functions.size(), 0);
        CountCalls(p, program.functions[static_cast<std::size_t>(work.back())].body, calls);
        work.pop_back();
        for (std::size_t f = 0; f < calls.size(); f++) {
            if (calls[f] > 0 && !live[f]) {
                live[f] = true;
                work.push_back(static_cast<std::int32_t>(f));
            }
        }
    }
    return live;
}

// by function: the interrupt handler and what it calls, which stay apart from the rest
std::vector<bool> HandlerCode(const Program& program) {
    std::vector<bool> reached(program.functions.size(), false);
    if (program.handler < 0) {
        return reached;
    }
    std::vector<std::int32_t> work = {program.handler};
    reached[static_cast<std::size_t>(program.handler)] = true;
    while (!work.empty()) {
        std::int32_t f = work.back();
        work.pop_back();
        for (std::int32_t callee : program.functions[static_cast<std::size_t>(f)].callees) {
            if (!reached[static_cast<std::size_t>(callee)]) {
                reached[static_cast<std::size_t>(callee)] = true;
                work.push_back(callee);
            }
        }
    }
    return reached;
}

// a function no code that runs calls (every call copied, or none to begin with) gives up its
// body: the backend's counts over the whole program (FindInlines, FindDivPairs) no longer see
// the calls and divisions in it, and the passes skip it. Not main, the handler, a function
// whose address is taken, or one whose type has no literal
std::uint64_t EmptyDeadBodies(Pass& p) {
    std::vector<bool> live = Live(p);
    std::uint64_t changes = 0;
    for (std::size_t f = 0; f < live.size(); f++) {
        Function& fn = p.program->functions[f];
        TypeKind kind = KindOf(*p.program, fn.ret);
        bool literal = fn.ret == kVoidType || IsIntegerKind(kind) || kind == TypeKind::Bool ||
                       kind == TypeKind::Enum || kind == TypeKind::Ptr;
        if (live[f] || fn.body < 0 || fn.address_taken || fn.dispatcher || !literal) {
            continue;
        }
        const Expr& body = At(p, fn.body);
        // (constant folding makes (do 0) a 0)
        bool empty = body.kind == ExprKind::Literal ||
                     (body.kind == ExprKind::Do &&
                      (body.count == 0 ||
                       (body.count == 1 && At(p, Item(p, body, 0)).kind == ExprKind::Literal)));
        if (empty || !Allow(p, "body of a function nothing calls", fn.body, -1)) {
            continue;
        }
        std::vector<ExprId> items;
        if (fn.ret != kVoidType) {
            Expr zero;
            zero.kind = ExprKind::Literal;
            zero.type = fn.ret;
            zero.loc = body.loc;
            items.push_back(NewExpr(p, zero));
        }
        ExprId old = fn.body;
        ExprId made = NewList(p, ExprKind::Do, fn.ret, items, At(p, old).loc);
        Retain(p, made);
        p.program->functions[f].body = made;
        Release(p, old);
        changes++;
    }
    return changes;
}

// one step: the calls of the functions picked now copied, but those in a function itself
// copied in this step (its copies make them calls of their own, counted at the next)
std::uint64_t InlineStep(Pass& p) {
    const Program& program = *p.program;
    std::size_t count = program.functions.size();
    std::vector<bool> runs = Live(p);
    std::vector<bool> handler = HandlerCode(program);
    std::vector<std::uint32_t> calls(count, 0);
    for (std::size_t f = 0; f < count; f++) {
        if (runs[f]) {
            CountCalls(p, program.functions[f].body, calls);
        }
    }
    Inliner in;
    in.p = &p;
    in.copied.assign(count, false);
    in.waits.assign(count, false);
    in.once.assign(count, false);
    in.as_value.assign(count, false);
    in.size.assign(count, 0);
    in.budget = kMaxCopied > p.inline_copied ? kMaxCopied - p.inline_copied : 0;
    for (std::size_t f = 0; f < count; f++) {
        const Function& fn = program.functions[f];
        bool never = std::find(p.options->no_inline.begin(), p.options->no_inline.end(), fn.name) !=
                     p.options->no_inline.end();
        // a function whose address is taken keeps its body to itself (its calls through
        // pointers are not these), as does the handler's code, and one with a with_aux of its
        // own (FindInlines)
        if (calls[f] == 0 || never || static_cast<std::int32_t>(f) == program.main || fn.body < 0 ||
            fn.address_taken || fn.dispatcher || handler[f] ||
            HasAuxSwitch(program, static_cast<std::int32_t>(f))) {
            continue;
        }
        in.size[f] = CountExprs(program, fn.body);
        bool small = in.size[f] <= kMaxInlineExprs;
        // a small function with a loop, copied into a caller the backend inlines once, would
        // give that caller a loop, and the backend keeps it a JSR in a loop then (snake's step
        // and speaker.buzz)
        if (small && calls[f] > 1 && HasLoops(program, fn.body)) {
            continue;
        }
        std::int32_t id = static_cast<std::int32_t>(f);
        in.as_value[f] = (small || calls[f] == 1) && Lowerable(p, id, CopyMode::Value);
        if (in.as_value[f] || ((small || calls[f] == 1) && Lowerable(p, id, CopyMode::Statement))) {
            in.copied[f] = true;
            in.once[f] = !small;
        }
    }
    // a function copied in this step keeps its calls for the next, when the copies count
    // (only one some call of which may be copied: else its calls would wait for ever)
    in.wanted.assign(count, 0);
    in.dry = true;
    for (std::size_t f = 0; f < count; f++) {
        if (runs[f] && !handler[f]) {
            in.function = static_cast<std::int32_t>(f);
            Visit(in, p.program->functions[f].body, false);
        }
    }
    in.dry = false;
    for (std::size_t f = 0; f < count; f++) {
        in.waits[f] = in.copied[f] && in.wanted[f] > 0;
    }
    std::uint32_t budget = in.budget;
    for (std::size_t f = 0; f < count; f++) {
        if (!runs[f] || in.waits[f] || handler[f]) {
            continue;
        }
        in.function = static_cast<std::int32_t>(f);
        Visit(in, p.program->functions[f].body, false);
    }
    p.inline_copied += budget - in.budget;
    return in.changes;
}

} // namespace

std::uint64_t InlineCalls(Pass& p) {
    if (p.program->main < 0) {
        return 0;
    }
    // the steps go down the call tree, a level each (the summaries still hold: a copy does what
    // its call did)
    std::uint64_t changes = 0;
    for (int step = 0; step < kMaxSteps; step++) {
        std::uint64_t made = InlineStep(p);
        changes += made;
        if (made == 0) {
            break;
        }
    }
    if (!p.options->keep_bodies) {
        changes += EmptyDeadBodies(p);
    }
    return changes;
}

} // namespace tisple::opt
