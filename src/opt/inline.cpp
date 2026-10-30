// Inlining (docs/midlevel-optimizer.md, slice 6): a call becomes a copy of its function's body
// where it is made, so the other passes see what it does there: a loop whose passes call a
// function that does the same each time collapses (161, 585), the globals a caller just set are
// constants in the copy (1134), a store the callee overwrites is dead (608).
// - Which calls: those the backend inlines anyway (FindInlines, backend/analysis.cpp), so the
//   code stays about the size it was: every call of a function of at most kMaxInlineExprs
//   nodes, and the one call of a function called in one place, unless that call is in a loop
//   and the function has loops of its own (the backend keeps those a JSR). Never main nor what
//   --no-inline names (the race's clock ends), and only with a main: with none, any function
//   may be called from outside. Calls are counted in the functions that run (FunctionsThatRun):
//   a function whose calls were all copied runs no more, and the calls in its body do not count.
// - The copy: a let of a new local per parameter, its argument as the value, then the body, its
//   lets and fors on new locals of the caller. A parameter the body never sets reads its
//   argument in place when that is a variable the call cannot change (CanBind in the backend).
// - Returns: the body's last item (return v) is v; an (if c ...) among its items one of whose
//   arms always returns makes the rest of the body the other arm:
//     (do S1 (if c (return a)) S2 b)  ->  (do S1 (if c a (do S2 b)))
//   A return anywhere else (in a loop, a match, a value, an arm that may or may not return)
//   keeps the function a function at that call.
// - A call that is a statement gives its items to the do around it, as does (set x (f ...)) of
//   a plain variable, x set where each path through the body ends; any other call becomes a do
//   in its place. A call whose function is copied in this round waits for the next, when the
//   copies count as its calls (as the backend counts them).
// The function stays as it was (tests call functions by address); the interpreter runs the
// program as written, and the frames keep their order (Function::callees is left as it is).

#include "backend/analysis.hpp"
#include "opt/collapse.hpp"
#include "opt/licm.hpp"
#include "opt/ranges.hpp"

#include <algorithm>

namespace tisple::opt {

namespace {

// nodes copied in one compile at most (the compile stays fast)
constexpr std::uint32_t kMaxCopied = 30000;

enum class Mode : std::uint8_t {
    Value,     // the copy's value is the call's
    Statement, // nobody uses it
    SetVar,    // it goes into a variable
};

bool HasReturn(const Pass& p, ExprId id) {
    if (id < 0) {
        return false;
    }
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::Return) {
        return true;
    }
    bool found = false;
    EachChild(p, e, [&](ExprId child) { found = found || HasReturn(p, child); });
    return found;
}

// every way through id ends in a return
bool AlwaysReturns(const Pass& p, ExprId id) {
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Return:
        return true;
    case ExprKind::Do:
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (AlwaysReturns(p, Item(p, e, i))) {
                return true;
            }
        }
        return false;
    case ExprKind::If:
        return e.c >= 0 && AlwaysReturns(p, e.b) && AlwaysReturns(p, e.c);
    default:
        return false;
    }
}

// one copy of a body being made, or (build false) only looked at
struct Copier {
    Pass* p = nullptr;
    std::int32_t caller = -1;
    std::unordered_map<std::int32_t, std::int32_t> names; // callee var -> caller var
    TypeId type = kVoidType;                              // the callee's return type
    Mode mode = Mode::Value;
    std::int32_t dest = -1; // SetVar: the variable
    bool build = false;
    bool ok = true;
};

ExprId Copy(Copier& c, ExprId id) {
    return c.build ? CloneExpr(*c.p, id, c.caller, c.names) : -1;
}

// what a value at the end of the body becomes (v -1: a return with none)
void Finish(Copier& c, ExprId v, std::vector<ExprId>& out) {
    if (v < 0 || !c.build) {
        return;
    }
    ExprId copy = Copy(c, v);
    if (c.mode != Mode::SetVar) {
        out.push_back(copy);
        return;
    }
    Expr place;
    place.kind = ExprKind::VarRef;
    place.type = VarOf(*c.p, c.dest).type;
    place.ref = c.dest;
    place.loc = At(*c.p, v).loc;
    Expr set;
    set.kind = ExprKind::Set;
    set.type = kVoidType;
    set.a = NewExpr(*c.p, place);
    set.b = copy;
    set.loc = place.loc;
    out.push_back(NewExpr(*c.p, set));
}

TypeId ResultType(const Copier& c) {
    return c.mode == Mode::Value ? c.type : kVoidType;
}

ExprId MakeDo(Copier& c, const std::vector<ExprId>& out, const Loc& loc) {
    return c.build ? NewList(*c.p, ExprKind::Do, ResultType(c), out, loc) : -1;
}

// items, the rest of the body from some point, with no return left: a do of their copies
ExprId Lower(Copier& c, std::vector<ExprId> items, Loc loc) {
    const Pass& p = *c.p;
    std::vector<ExprId> out;
    for (std::size_t k = 0; k < items.size() && c.ok; k++) {
        ExprId id = items[k];
        bool last = k + 1 == items.size();
        if (!HasReturn(p, id)) {
            if (last) {
                Finish(c, id, out);
            } else if (c.build) {
                out.push_back(Copy(c, id));
            }
            continue;
        }
        Expr e = At(p, id);
        if (e.kind == ExprKind::Return) {
            Finish(c, e.a, out);
            return MakeDo(c, out, loc);
        }
        if (e.kind == ExprKind::Do) {
            // its items join these (its lets get new locals, so a longer scope does no harm)
            std::vector<ExprId> inner(p.program->lists.begin() + e.first,
                                      p.program->lists.begin() + e.first + e.count);
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(k));
            items.insert(items.begin() + static_cast<std::ptrdiff_t>(k), inner.begin(),
                         inner.end());
            k--;
            continue;
        }
        if (e.kind != ExprKind::If || HasReturn(p, e.a)) {
            c.ok = false;
            return -1;
        }
        bool then_returns = AlwaysReturns(p, e.b);
        bool else_returns = e.c >= 0 && AlwaysReturns(p, e.c);
        bool then_some = HasReturn(p, e.b);
        bool else_some = e.c >= 0 && HasReturn(p, e.c);
        if ((then_some && !then_returns) || (else_some && !else_returns)) {
            c.ok = false; // an arm that may or may not return: the rest would need two copies
            return -1;
        }
        std::vector<ExprId> rest(items.begin() + static_cast<std::ptrdiff_t>(k) + 1, items.end());
        std::vector<ExprId> a = {e.b};
        std::vector<ExprId> b;
        if (e.c >= 0) {
            b.push_back(e.c);
        }
        if (!then_returns) {
            a.insert(a.end(), rest.begin(), rest.end());
        }
        if (!else_returns) {
            b.insert(b.end(), rest.begin(), rest.end());
        }
        ExprId cond = Copy(c, e.a);
        ExprId arm_a = Lower(c, a, At(p, e.b).loc);
        ExprId arm_b = Lower(c, b, e.loc);
        if (c.build && c.ok) {
            Expr branch;
            branch.kind = ExprKind::If;
            branch.type = ResultType(c);
            branch.a = cond;
            branch.b = arm_a;
            branch.c = arm_b;
            branch.loc = e.loc;
            out.push_back(NewExpr(*c.p, branch));
        }
        return MakeDo(c, out, loc);
    }
    return MakeDo(c, out, loc);
}

// may the body become a copy with no returns (looked at without building one)
bool Lowerable(Pass& p, std::int32_t callee) {
    const Function& fn = p.program->functions[static_cast<std::size_t>(callee)];
    const Expr& body = At(p, fn.body);
    if (body.kind != ExprKind::Do) {
        return false;
    }
    Copier c;
    c.p = &p;
    c.type = fn.ret;
    std::vector<ExprId> items(p.program->lists.begin() + body.first,
                              p.program->lists.begin() + body.first + body.count);
    Lower(c, items, body.loc);
    return c.ok;
}

// an argument read where the parameter is (a few nodes copied to each read) at most this
// many nodes in all
constexpr int kMaxPutIn = 8;

int Nodes(const Pass& p, ExprId id) {
    int n = 1;
    EachChild(p, At(p, id), [&](ExprId child) { n += Nodes(p, child); });
    return n;
}

// reads of var under id; under a loop of the body they count as many
int Reads(const Pass& p, ExprId id, std::int32_t var, int loops) {
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::VarRef && e.ref == var) {
        return loops > 0 ? kMaxPutIn + 1 : 1;
    }
    bool loop = e.kind == ExprKind::While || e.kind == ExprKind::For;
    int n = 0;
    EachChild(p, e, [&](ExprId child) { n += Reads(p, child, var, loops + (loop ? 1 : 0)); });
    return n;
}

// nothing the body does changes what id reads
bool Unchanged(const Pass& p, const Summary& s, ExprId id) {
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::VarRef) {
        const Var& v = VarOf(p, e.ref);
        bool global = v.kind == VarKind::Global && s.writes[static_cast<std::size_t>(e.ref)];
        return !global && !(v.address_taken && s.stores) && !Hardware(v);
    }
    bool same = true;
    EachChild(p, e, [&](ExprId child) { same = same && Unchanged(p, s, child); });
    return same;
}

// may the argument stand where the body reads the parameter, as the backend binds an
// inlined call's arguments (CanBind): the parameter never set, its address not taken; the
// argument pure and quiet, reading nothing the body or the later arguments change; a literal
// or a variable for any number of reads, else a few nodes read a few times (not in a loop)
bool PutIn(const Pass& p, std::int32_t callee, ExprId call, std::uint32_t i) {
    const Function& fn = p.program->functions[static_cast<std::size_t>(callee)];
    std::int32_t param = fn.params[i];
    ExprId arg = Item(p, At(p, call), i);
    const Summary& s = p.summaries[static_cast<std::size_t>(callee)];
    if (VarOf(p, param).address_taken || CountSets(p, fn.body, param) > 0 || s.jsr ||
        s.unknown || !IsPure(p, arg) || !Quiet(p, arg) || !Unchanged(p, s, arg)) {
        return false;
    }
    ExprKind kind = At(p, arg).kind;
    if (kind != ExprKind::Literal && kind != ExprKind::VarRef &&
        Nodes(p, arg) * Reads(p, fn.body, param, 0) > kMaxPutIn) {
        return false;
    }
    for (std::uint32_t j = i + 1; j < At(p, call).count; j++) {
        if (!IsPure(p, Item(p, At(p, call), j))) {
            return false;
        }
    }
    return true;
}

// every read of var under id becomes a copy of value
void PutValue(Pass& p, ExprId id, std::int32_t var, ExprId value) {
    Expr e = At(p, id);
    if (e.kind == ExprKind::VarRef && e.ref == var) {
        ExprId copy = CloneExpr(p, value);
        Retain(p, copy);
        ReplaceWith(p, id, copy);
        Release(p, copy);
        return;
    }
    EachChild(p, e, [&](ExprId child) { PutValue(p, child, var, value); });
}

// the copy for a call: its lets of parameters, then the body's items, into out
void CopyCall(Pass& p, std::int32_t caller, ExprId call, Mode mode, std::int32_t dest,
              std::vector<ExprId>& out) {
    Expr e = At(p, call);
    const Function& fn = p.program->functions[static_cast<std::size_t>(e.ref)];
    Copier c;
    c.p = &p;
    c.caller = caller;
    c.type = fn.ret;
    c.mode = mode;
    c.dest = dest;
    c.build = true;
    std::vector<std::pair<std::int32_t, ExprId>> put; // parameters read as their arguments
    for (std::uint32_t i = 0; i < e.count && i < fn.params.size(); i++) {
        std::int32_t param = fn.params[i];
        ExprId arg = Item(p, e, i);
        if (PutIn(p, e.ref, call, i)) {
            if (At(p, arg).kind == ExprKind::VarRef) {
                c.names[param] = At(p, arg).ref;
            } else {
                put.push_back({param, arg});
            }
            continue;
        }
        std::int32_t local = CopyLocal(p, caller, param);
        p.program->vars[static_cast<std::size_t>(local)].kind = VarKind::Local;
        c.names[param] = local;
        Expr let;
        let.kind = ExprKind::Let;
        let.type = kVoidType;
        let.ref = local;
        let.a = arg; // the call lets go of it once replaced
        let.loc = At(p, arg).loc;
        out.push_back(NewExpr(p, let));
    }
    const Expr& body = At(p, fn.body);
    std::vector<ExprId> items(p.program->lists.begin() + body.first,
                              p.program->lists.begin() + body.first + body.count);
    ExprId lowered = Lower(c, items, e.loc);
    for (const auto& [param, arg] : put) {
        PutValue(p, lowered, param, arg);
    }
    const Expr& d = At(p, lowered);
    out.insert(out.end(), p.program->lists.begin() + d.first,
               p.program->lists.begin() + d.first + d.count);
    // the do made for the items names them; out names them now
    for (ExprId item : out) {
        Retain(p, item);
    }
    Retain(p, lowered);
    Release(p, lowered);
}

struct Inliner {
    Pass* p = nullptr;
    std::int32_t function = -1;   // being walked
    std::vector<bool> copied;     // by function: its calls are copied in this round
    std::vector<bool> waits;      // by function: its body is copied in this round
    std::vector<bool> once;       // by function: called in one place, not small
    std::vector<bool> has_loops;  // by function
    std::uint32_t budget = 0;     // nodes left to copy
    std::vector<std::uint32_t> size;
    int loops = 0;                // loops around the node
    std::uint64_t changes = 0;
};

// may this call be copied here
bool Wanted(Inliner& in, ExprId call, bool shared) {
    Pass& p = *in.p;
    const Expr& e = At(p, call);
    std::size_t f = static_cast<std::size_t>(e.ref);
    if (e.kind != ExprKind::Call || shared || Shared(p, call) || !in.copied[f] ||
        in.waits[static_cast<std::size_t>(in.function)] || in.size[f] > in.budget) {
        return false;
    }
    if (in.once[f] && in.loops > 0 && in.has_loops[f]) {
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
    CopyCall(p, in.function, call, Mode::Value, -1, items);
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
        Mode mode = Mode::Statement;
        ExprId call = item;
        std::int32_t dest = -1;
        if (e.kind == ExprKind::Set && At(p, e.a).kind == ExprKind::VarRef && !Shared(p, e.a) &&
            !Hardware(VarOf(p, At(p, e.a).ref))) {
            mode = Mode::SetVar;
            call = e.b;
            dest = At(p, e.a).ref;
        }
        if (value || shared || Shared(p, item) || At(p, call).kind != ExprKind::Call ||
            !Wanted(in, call, shared)) {
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
    if (e.kind == ExprKind::Call && Wanted(in, id, shared)) {
        InPlace(in, id);
        return;
    }
    bool loop = e.kind == ExprKind::While || e.kind == ExprKind::For;
    in.loops += loop ? 1 : 0;
    EachChild(p, e, [&](ExprId child) { Visit(in, child, shared); });
    in.loops -= loop ? 1 : 0;
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

} // namespace

std::uint64_t InlineCalls(Pass& p) {
    const Program& program = *p.program;
    if (program.main < 0) {
        return 0;
    }
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
    in.has_loops.assign(count, false);
    in.size.assign(count, 0);
    in.budget = kMaxCopied > p.inline_copied ? kMaxCopied - p.inline_copied : 0;
    for (std::size_t f = 0; f < count; f++) {
        const Function& fn = program.functions[f];
        bool never = std::find(p.options->no_inline.begin(), p.options->no_inline.end(),
                               fn.name) != p.options->no_inline.end();
        // a function whose address is taken keeps its body to itself (its calls through
        // pointers are not these), as does the handler's code
        if (calls[f] == 0 || never || static_cast<std::int32_t>(f) == program.main ||
            fn.body < 0 || fn.address_taken || fn.dispatcher || handler[f]) {
            continue;
        }
        in.size[f] = CountExprs(program, fn.body);
        bool small = in.size[f] <= kMaxInlineExprs;
        if ((small || calls[f] == 1) && Lowerable(p, static_cast<std::int32_t>(f))) {
            in.copied[f] = true;
            in.once[f] = !small;
            in.has_loops[f] = HasLoops(program, fn.body);
        }
    }
    // a function copied this round keeps its calls for the next, when the copies count
    in.waits = in.copied;
    std::uint32_t budget = in.budget;
    for (std::size_t f = 0; f < count; f++) {
        if (!runs[f] || in.waits[f] || handler[f]) {
            continue;
        }
        in.function = static_cast<std::int32_t>(f);
        Visit(in, p.program->functions[f].body, false);
    }
    p.inline_copied += budget - in.budget;
    if (!p.options->keep_bodies) {
        in.changes += EmptyDeadBodies(p);
    }
    return in.changes;
}

} // namespace tisple::opt
