// Editing the tree in place. Nodes may be shared (constant folding leaves some with two
// parents), so every node keeps a count of who names it: a node is only rewritten when one
// parent names it, and a node nobody names any more gives up its children and becomes a
// plain literal, as fold.cpp does, so backend passes that scan every expression see only the
// live tree.

#include "opt/opt_internal.hpp"

namespace tisple::opt {

namespace {

template <typename Fn> void EachChild(const Pass& p, const Expr& e, Fn fn) {
    for (ExprId child : {e.a, e.b, e.c}) {
        if (child >= 0) {
            fn(child);
        }
    }
    if (e.kind == ExprKind::Call || e.kind == ExprKind::StructLit || e.kind == ExprKind::Do) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            fn(Item(p, e, i));
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            fn(p.program->clauses[e.first + i].body);
        }
    }
}

void Retain(Pass& p, ExprId id) {
    p.refs[static_cast<std::size_t>(id)]++;
}

void Release(Pass& p, ExprId id) {
    std::uint32_t& refs = p.refs[static_cast<std::size_t>(id)];
    if (refs == 0 || --refs > 0) {
        return;
    }
    Expr old = At(p, id);
    EachChild(p, old, [&](ExprId child) { Release(p, child); });
    Expr dead;
    dead.kind = ExprKind::Literal;
    dead.type = old.type;
    dead.loc = old.loc;
    p.program->exprs[static_cast<std::size_t>(id)] = dead;
}

ExprId NewEmptyDo(Pass& p, const Loc& loc) {
    Expr e;
    e.kind = ExprKind::Do;
    e.type = kVoidType;
    e.loc = loc;
    ExprId id = static_cast<ExprId>(p.program->exprs.size());
    p.program->exprs.push_back(e);
    p.refs.push_back(1);
    return id;
}

struct Editor {
    Pass* p = nullptr;
    const Liveness* live = nullptr;
    Decide decide = nullptr;
    std::uint64_t changes = 0;
};

// the statement at slot (an arm or clause body): rewritten when the pass says so
void EditSlot(Editor& ed, ExprId& slot) {
    Pass& p = *ed.p;
    ExprId stmt = slot;
    const Expr& e = At(p, stmt);
    if (e.kind == ExprKind::Do && e.count == 0) {
        return; // already nothing
    }
    Action a = ed.decide(p, *ed.live, stmt);
    if (a.kind == Action::Keep || !Allow(p, a.what, stmt, a.var)) {
        return;
    }
    if (a.kind == Action::Drop) {
        ExprId empty = NewEmptyDo(p, e.loc);
        slot = empty;
    } else {
        Retain(p, a.with);
        slot = a.with;
    }
    Release(p, stmt);
    ed.changes++;
}

// the items of a do whose values are unused; the last item of a do with a value stays
void EditDo(Editor& ed, ExprId id) {
    Pass& p = *ed.p;
    Expr d = At(p, id);
    std::vector<ExprId> items;
    bool changed = false;
    for (std::uint32_t i = 0; i < d.count; i++) {
        ExprId item = Item(p, d, i);
        bool value = i + 1 == d.count && d.type != kVoidType;
        Action a;
        if (!value) {
            a = ed.decide(p, *ed.live, item);
        }
        if (a.kind == Action::Keep || !Allow(p, a.what, item, a.var)) {
            items.push_back(item);
            continue;
        }
        if (a.kind == Action::Replace) {
            Retain(p, a.with);
            items.push_back(a.with);
        }
        Release(p, item);
        changed = true;
        ed.changes++;
    }
    if (!changed) {
        return;
    }
    Expr& out = p.program->exprs[static_cast<std::size_t>(id)];
    out.first = static_cast<std::uint32_t>(p.program->lists.size());
    out.count = static_cast<std::uint32_t>(items.size());
    p.program->lists.insert(p.program->lists.end(), items.begin(), items.end());
}

bool Shared(const Pass& p, ExprId id) {
    return p.refs[static_cast<std::size_t>(id)] != 1;
}

// finds the statements under id, outermost first; used: id's value is needed
void Visit(Editor& ed, ExprId id, bool used) {
    if (id < 0) {
        return;
    }
    Pass& p = *ed.p;
    ExprKind kind = At(p, id).kind;
    switch (kind) {
    case ExprKind::Do: {
        if (!Shared(p, id)) {
            EditDo(ed, id);
        }
        Expr e = At(p, id);
        for (std::uint32_t i = 0; i < e.count; i++) {
            Visit(ed, Item(p, e, i), used && i + 1 == e.count);
        }
        return;
    }
    case ExprKind::If: {
        Visit(ed, At(p, id).a, true);
        bool statement = !used && At(p, id).type == kVoidType && !Shared(p, id);
        for (int arm = 0; arm < 2; arm++) {
            ExprId slot = arm == 0 ? At(p, id).b : At(p, id).c;
            if (slot < 0) {
                continue;
            }
            if (statement) {
                EditSlot(ed, slot);
                Expr& e = p.program->exprs[static_cast<std::size_t>(id)];
                (arm == 0 ? e.b : e.c) = slot;
            }
            Visit(ed, slot, used);
        }
        return;
    }
    case ExprKind::Match: {
        Expr e = At(p, id);
        Visit(ed, e.a, true);
        bool statement = !used && e.type == kVoidType && !Shared(p, id);
        for (std::uint32_t i = 0; i < e.count; i++) {
            ExprId slot = p.program->clauses[e.first + i].body;
            if (statement) {
                EditSlot(ed, slot);
                p.program->clauses[e.first + i].body = slot;
            }
            Visit(ed, slot, used);
        }
        return;
    }
    case ExprKind::While: {
        Expr e = At(p, id);
        Visit(ed, e.a, true);
        Visit(ed, e.b, false);
        return;
    }
    case ExprKind::For: {
        Expr e = At(p, id);
        Visit(ed, e.a, true);
        Visit(ed, e.b, true);
        Visit(ed, e.c, false);
        return;
    }
    case ExprKind::Literal:
    case ExprKind::VarRef:
    case ExprKind::Break:
    case ExprKind::Continue:
        return;
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
    case ExprKind::Call:
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
    case ExprKind::Jsr:
    case ExprKind::JsrRegs:
    case ExprKind::StructLit:
    case ExprKind::Return:
    case ExprKind::Let:
    case ExprKind::Set: {
        Expr e = At(p, id);
        Visit(ed, e.a, true);
        Visit(ed, e.b, true);
        Visit(ed, e.c, true);
        if (kind == ExprKind::Call || kind == ExprKind::StructLit) {
            for (std::uint32_t i = 0; i < e.count; i++) {
                Visit(ed, Item(p, e, i), true);
            }
        }
        return;
    }
    default:
        return; // not known here: left as it is
    }
}

} // namespace

void CountRefs(Pass& p) {
    p.refs.assign(p.program->exprs.size(), 0);
    for (const Expr& e : p.program->exprs) {
        EachChild(p, e, [&](ExprId child) { Retain(p, child); });
    }
    for (const Function& fn : p.program->functions) {
        if (fn.body >= 0) {
            Retain(p, fn.body);
        }
    }
}

std::uint64_t EditStatements(Pass& p, std::int32_t function, Decide decide) {
    const Function& fn = p.program->functions[static_cast<std::size_t>(function)];
    if (fn.body < 0) {
        return 0;
    }
    Liveness live = ComputeLiveness(p, function);
    Editor ed;
    ed.p = &p;
    ed.live = &live;
    ed.decide = decide;
    Visit(ed, fn.body, fn.ret != kVoidType);
    return ed.changes;
}

} // namespace tisple::opt
