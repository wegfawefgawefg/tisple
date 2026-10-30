// Effect summaries: for each function, which globals it may read and write and whether it
// reads or writes through pointers, does I/O or jsr, callees included. By pointer provenance
// (docs/language.md) a pointer reaches only address-taken variables and memory the program
// does not declare, so loads and stores never touch a plain global. Each set rooted at a
// global writes it; partial sets (s.x, (at a i)) count as writes of the whole variable.

#include "opt/opt_internal.hpp"

namespace tisple::opt {

namespace {

bool IsGlobal(const Pass& p, std::int32_t var) {
    return VarOf(p, var).kind == VarKind::Global;
}

void Merge(Summary& into, const Summary& from) {
    for (std::size_t v = 0; v < into.reads.size(); v++) {
        into.reads[v] = into.reads[v] || from.reads[v];
        into.writes[v] = into.writes[v] || from.writes[v];
    }
    into.loads = into.loads || from.loads;
    into.stores = into.stores || from.stores;
    into.io = into.io || from.io;
    into.jsr = into.jsr || from.jsr;
    into.unknown = into.unknown || from.unknown;
}

void Scan(const Pass& p, Summary& s, ExprId id);

// a place: its index expressions are read, its root written (or, for addr_of, neither)
void ScanPlace(const Pass& p, Summary& s, ExprId id, bool write) {
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::VarRef:
        if (write && IsGlobal(p, e.ref)) {
            s.writes[static_cast<std::size_t>(e.ref)] = true;
        }
        return;
    case ExprKind::Index:
        ScanPlace(p, s, e.a, write);
        Scan(p, s, e.b);
        return;
    case ExprKind::Field:
        ScanPlace(p, s, e.a, write);
        return;
    case ExprKind::Load:
    case ExprKind::LoadField:
        // the base of a place read through a pointer: only the pointer is worked out here
        Scan(p, s, e.a);
        return;
    default:
        Scan(p, s, id);
        return;
    }
}

void ScanItems(const Pass& p, Summary& s, const Expr& e) {
    for (std::uint32_t i = 0; i < e.count; i++) {
        Scan(p, s, Item(p, e, i));
    }
}

void Scan(const Pass& p, Summary& s, ExprId id) {
    if (id < 0) {
        return;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Literal:
    case ExprKind::FnRef:
    case ExprKind::Break:
    case ExprKind::Continue:
        return;
    case ExprKind::CallPtr:
        // every function the pointer may hold (they are callees, so already summed up)
        for (std::int32_t target : PointerTargets(p, At(p, e.a).type)) {
            Merge(s, p.summaries[static_cast<std::size_t>(target)]);
        }
        s.unknown = s.unknown || PointerTargets(p, At(p, e.a).type).empty();
        Scan(p, s, e.a);
        ScanItems(p, s, e);
        return;
    case ExprKind::VarRef:
        if (IsGlobal(p, e.ref)) {
            s.reads[static_cast<std::size_t>(e.ref)] = true;
        }
        return;
    case ExprKind::Set:
        ScanPlace(p, s, e.a, true);
        Scan(p, s, e.b);
        return;
    case ExprKind::AddrOf:
        ScanPlace(p, s, e.a, false);
        return;
    case ExprKind::Load:
    case ExprKind::LoadField:
        s.loads = true;
        Scan(p, s, e.a);
        return;
    case ExprKind::Store:
    case ExprKind::StoreField:
        s.stores = true;
        break;
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
    case ExprKind::Interrupts:
        s.io = true;
        break;
    case ExprKind::Jsr:
        s.jsr = true;
        break;
    case ExprKind::Call:
        Merge(s, p.summaries[static_cast<std::size_t>(e.ref)]);
        ScanItems(p, s, e);
        return;
    case ExprKind::StructLit:
    case ExprKind::Do:
        ScanItems(p, s, e);
        return;
    case ExprKind::Match:
        Scan(p, s, e.a);
        for (std::uint32_t i = 0; i < e.count; i++) {
            Scan(p, s, p.program->clauses[e.first + i].body);
        }
        return;
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::And:
    case ExprKind::Or:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::JsrRegs:
    case ExprKind::If:
    case ExprKind::While:
    case ExprKind::For:
    case ExprKind::Return:
    case ExprKind::Let:
        break;
    default:
        // not known here: it may do anything
        s.unknown = true;
        break;
    }
    Scan(p, s, e.a);
    Scan(p, s, e.b);
    Scan(p, s, e.c);
}

// functions in an order where every callee comes before its callers
std::vector<std::int32_t> CalleesFirst(const Program& program) {
    std::size_t n = program.functions.size();
    std::vector<std::uint32_t> pending(n, 0);
    std::vector<std::vector<std::int32_t>> callers(n);
    for (std::size_t f = 0; f < n; f++) {
        for (std::int32_t callee : program.functions[f].callees) {
            pending[f]++;
            callers[static_cast<std::size_t>(callee)].push_back(static_cast<std::int32_t>(f));
        }
    }
    std::vector<std::int32_t> order;
    for (std::size_t f = 0; f < n; f++) {
        if (pending[f] == 0) {
            order.push_back(static_cast<std::int32_t>(f));
        }
    }
    for (std::size_t i = 0; i < order.size(); i++) {
        for (std::int32_t caller : callers[static_cast<std::size_t>(order[i])]) {
            if (--pending[static_cast<std::size_t>(caller)] == 0) {
                order.push_back(caller);
            }
        }
    }
    return order;
}

} // namespace

void ComputeSummaries(Pass& p) {
    const Program& program = *p.program;
    std::size_t vars = program.vars.size();
    p.targets.clear();
    p.pointer_summaries.clear();
    for (const Expr& e : program.exprs) {
        if (e.kind == ExprKind::CallPtr) {
            TypeId type = program.exprs[static_cast<std::size_t>(e.a)].type;
            if (p.targets.count(type) == 0) {
                p.targets.emplace(type, CallTargets(program, type));
            }
        }
    }
    p.summaries.assign(program.functions.size(), Summary());
    for (Summary& s : p.summaries) {
        s.reads.assign(vars, false);
        s.writes.assign(vars, false);
        // until its body is seen (a call graph the checker let through with a cycle)
        s.unknown = true;
        s.terminates = false;
    }
    for (std::int32_t f : CalleesFirst(program)) {
        Summary& s = p.summaries[static_cast<std::size_t>(f)];
        s.unknown = false;
        ExprId body = program.functions[static_cast<std::size_t>(f)].body;
        Scan(p, s, body);
        s.terminates = CodeTerminates(p, body);
        bool writes = false;
        for (std::size_t v = 0; v < vars && !writes; v++) {
            writes = s.writes[v];
        }
        s.pure = !writes && !s.stores && !s.io && !s.jsr && !s.unknown && s.terminates;
    }
    for (const auto& [type, targets] : p.targets) {
        Summary u;
        u.reads.assign(vars, false);
        u.writes.assign(vars, false);
        u.unknown = targets.empty();
        u.pure = !targets.empty();
        for (std::int32_t target : targets) {
            const Summary& s = p.summaries[static_cast<std::size_t>(target)];
            Merge(u, s);
            u.terminates = u.terminates && s.terminates;
            u.pure = u.pure && s.pure;
        }
        p.pointer_summaries.emplace(type, u);
    }
}

bool PureFunction(const Pass& p, std::int32_t function) {
    return p.summaries[static_cast<std::size_t>(function)].pure;
}

const std::vector<std::int32_t>& PointerTargets(const Pass& p, TypeId type) {
    static const std::vector<std::int32_t> kNone;
    auto found = p.targets.find(type);
    return found == p.targets.end() ? kNone : found->second;
}

const Summary& CallSummary(const Pass& p, const Expr& call) {
    if (call.kind == ExprKind::Call) {
        return p.summaries[static_cast<std::size_t>(call.ref)];
    }
    // a pointer type no call of the program as summed up had: nothing known
    static Summary unknown;
    if (unknown.reads.size() != p.program->vars.size()) {
        unknown.reads.assign(p.program->vars.size(), true);
        unknown.writes.assign(p.program->vars.size(), true);
        unknown.loads = unknown.stores = unknown.io = unknown.jsr = unknown.unknown = true;
        unknown.terminates = false;
        unknown.pure = false;
    }
    auto found = p.pointer_summaries.find(At(p, call.a).type);
    return found == p.pointer_summaries.end() ? unknown : found->second;
}

std::int32_t PlaceRoot(const Pass& p, ExprId place) {
    while (place >= 0) {
        const Expr& e = At(p, place);
        if (e.kind == ExprKind::VarRef) {
            return e.ref;
        }
        if (e.kind != ExprKind::Index && e.kind != ExprKind::Field) {
            return -1;
        }
        place = e.a;
    }
    return -1;
}

bool PurePlace(const Pass& p, ExprId place) {
    const Expr& e = At(p, place);
    switch (e.kind) {
    case ExprKind::VarRef:
        return true;
    case ExprKind::Index:
        return PurePlace(p, e.a) && IsPure(p, e.b);
    case ExprKind::Field:
        return PurePlace(p, e.a);
    case ExprKind::Load:
    case ExprKind::LoadField:
        return IsPure(p, e.a);
    default:
        return IsPure(p, place);
    }
}

bool IsPure(const Pass& p, ExprId id) {
    if (id < 0) {
        return true;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Literal:
    case ExprKind::VarRef:
    case ExprKind::FnRef:
        return true;
    case ExprKind::CallPtr:
        if (!CallSummary(p, e).pure || !IsPure(p, e.a)) {
            return false;
        }
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (!IsPure(p, Item(p, e, i))) {
                return false;
            }
        }
        return true;
    case ExprKind::Field:
    case ExprKind::Load:
    case ExprKind::LoadField:
    case ExprKind::Unary:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
        return IsPure(p, e.a);
    case ExprKind::AddrOf:
        return PurePlace(p, e.a);
    case ExprKind::Index:
    case ExprKind::Binary:
    case ExprKind::And:
    case ExprKind::Or:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
        return IsPure(p, e.a) && IsPure(p, e.b);
    case ExprKind::If:
        return IsPure(p, e.a) && IsPure(p, e.b) && IsPure(p, e.c);
    case ExprKind::Call:
        if (!PureFunction(p, e.ref)) {
            return false;
        }
        [[fallthrough]];
    case ExprKind::StructLit:
    case ExprKind::Do:
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (!IsPure(p, Item(p, e, i))) {
                return false;
            }
        }
        return true;
    case ExprKind::Match:
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (!IsPure(p, p.program->clauses[e.first + i].body)) {
                return false;
            }
        }
        return IsPure(p, e.a);
    default:
        // writes, I/O, jsr, loops, exits, lets, and anything not known here
        return false;
    }
}

} // namespace tisple::opt
