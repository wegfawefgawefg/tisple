// Editing the tree in place. Nodes may be shared (constant folding leaves some with two
// parents), so every node keeps a count of who names it: a node is only rewritten when one
// parent names it, and a node nobody names any more gives up its children and becomes a
// plain literal, as fold.cpp does, so backend passes that scan every expression see only the
// live tree.

#include "opt/opt_internal.hpp"

namespace tisple::opt {

namespace {

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
        Retain(p, empty);
        slot = empty;
    } else {
        Retain(p, a.with);
        slot = a.with;
    }
    Release(p, stmt);
    ed.changes++;
}

// a return, break or continue: nothing after it in its do runs
bool Exits(const Pass& p, ExprId id) {
    ExprKind kind = At(p, id).kind;
    return kind == ExprKind::Return || kind == ExprKind::Break || kind == ExprKind::Continue;
}

// the items of a do whose values are unused; the last item of a do with a value stays. For
// the dead code pass, the items after an exit go too (so what they would do no longer counts
// in the function's summary)
void EditDo(Editor& ed, ExprId id) {
    Pass& p = *ed.p;
    Expr d = At(p, id);
    std::vector<ExprId> items;
    bool changed = false;
    bool exited = false;
    for (std::uint32_t i = 0; i < d.count; i++) {
        ExprId item = Item(p, d, i);
        bool value = i + 1 == d.count && d.type != kVoidType;
        Action a;
        if (exited && !value) {
            a.kind = Action::Drop;
            a.what = "statement after an exit";
        } else if (!value) {
            a = ed.decide(p, *ed.live, item);
        }
        exited = exited || (ed.decide == DecideDead && Exits(p, item));
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
    case ExprKind::FnRef:
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
    case ExprKind::CallPtr:
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
    case ExprKind::Interrupts:
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
        if (kind == ExprKind::Call || kind == ExprKind::CallPtr || kind == ExprKind::StructLit) {
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

bool Shared(const Pass& p, ExprId id) {
    return p.refs[static_cast<std::size_t>(id)] != 1;
}

ExprId NewExpr(Pass& p, const Expr& e) {
    ExprId id = static_cast<ExprId>(p.program->exprs.size());
    p.program->exprs.push_back(e);
    p.refs.push_back(0);
    EachChild(p, e, [&](ExprId child) { Retain(p, child); });
    return id;
}

ExprId NewEmptyDo(Pass& p, const Loc& loc) {
    Expr e;
    e.kind = ExprKind::Do;
    e.type = kVoidType;
    e.loc = loc;
    return NewExpr(p, e);
}

ExprId NewList(Pass& p, ExprKind kind, TypeId type, const std::vector<ExprId>& items,
               const Loc& loc) {
    Expr e;
    e.kind = kind;
    e.type = type;
    e.loc = loc;
    e.first = static_cast<std::uint32_t>(p.program->lists.size());
    e.count = static_cast<std::uint32_t>(items.size());
    p.program->lists.insert(p.program->lists.end(), items.begin(), items.end());
    return NewExpr(p, e);
}

void ReplaceWith(Pass& p, ExprId id, ExprId from) {
    Expr old = At(p, id);
    Expr with = At(p, from);
    EachChild(p, with, [&](ExprId child) { Retain(p, child); });
    with.loc = old.loc;
    p.program->exprs[static_cast<std::size_t>(id)] = with;
    EachChild(p, old, [&](ExprId child) { Release(p, child); });
}

void ReplaceWithLiteral(Pass& p, ExprId id, std::uint16_t bits) {
    Expr old = At(p, id);
    Expr lit;
    lit.kind = ExprKind::Literal;
    lit.type = old.type;
    lit.value = bits;
    lit.loc = old.loc;
    p.program->exprs[static_cast<std::size_t>(id)] = lit;
    EachChild(p, old, [&](ExprId child) { Release(p, child); });
}

void BecomeRead(Pass& p, ExprId id, std::int32_t var) {
    Expr old = At(p, id);
    Expr read;
    read.kind = ExprKind::VarRef;
    read.type = old.type;
    read.ref = var;
    read.loc = old.loc;
    p.program->exprs[static_cast<std::size_t>(id)] = read;
    EachChild(p, old, [&](ExprId child) { Release(p, child); });
}

ExprId MoveToLet(Pass& p, std::int32_t function, ExprId id, Loc loc, std::int32_t& var) {
    Expr value = At(p, id);
    var = NewLocal(p, function, value.type, value.loc);
    ExprId kept = NewExpr(p, value);
    BecomeRead(p, id, var);
    Expr let;
    let.kind = ExprKind::Let;
    let.type = kVoidType;
    let.ref = var;
    let.a = kept;
    let.loc = loc;
    return NewExpr(p, let);
}

std::int32_t NewLocal(Pass& p, std::int32_t function, TypeId type, const Loc& loc) {
    Function& fn = p.program->functions[static_cast<std::size_t>(function)];
    Var var;
    // a name no source can spell, so it never meets one
    var.name = "opt." + std::to_string(p.program->vars.size());
    var.kind = VarKind::Local;
    var.type = type;
    var.module = fn.module;
    var.function = function;
    var.loc = loc;
    var.made = true;
    std::int32_t id = static_cast<std::int32_t>(p.program->vars.size());
    p.program->vars.push_back(var);
    fn.locals.push_back(id);
    return id;
}

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
