// The interrupt handler, (fn name () void :interrupt), runs on every IRQ: between any two
// instructions of the rest of the program (docs/language.md, Interrupts).
//
// - It needs a main (the startup installs it) and shares no function with what main runs:
//   a static frame has one copy, so a function both run would see its frame overwritten in
//   the middle of a call.
// - (cli) in what it runs would let interrupts nest, which the frames do not allow.
// - What it reads and writes is shared with the rest of the program (Var::irq_shared): every
//   global it names; every variable whose address is taken when it loads or stores through a
//   pointer; every global and address-taken variable when it runs machine code.

#include "check/checker.hpp"

#include <string>

namespace tisple {

namespace {

// functions start runs: itself, its callees and theirs
std::vector<bool> Reached(const Program& program, std::int32_t start) {
    std::vector<bool> reached(program.functions.size(), false);
    if (start < 0) {
        return reached;
    }
    std::vector<std::int32_t> work = {start};
    reached[static_cast<std::size_t>(start)] = true;
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

struct Uses {
    std::vector<bool> globals; // by var: a global the handler names
    bool pointers = false;     // loads or stores through a pointer
    bool machine = false;      // jsr
    std::vector<Loc> nesting;  // (cli)
};

void Scan(const Program& program, ExprId id, Uses& u) {
    if (id < 0) {
        return;
    }
    const Expr& e = program.exprs[static_cast<std::size_t>(id)];
    switch (e.kind) {
    case ExprKind::VarRef:
        if (program.vars[static_cast<std::size_t>(e.ref)].kind == VarKind::Global) {
            u.globals[static_cast<std::size_t>(e.ref)] = true;
        }
        break;
    case ExprKind::Load:
    case ExprKind::LoadField:
    case ExprKind::Store:
    case ExprKind::StoreField:
        u.pointers = true;
        break;
    case ExprKind::Jsr:
        u.machine = true;
        break;
    case ExprKind::Interrupts:
        if (e.value != 0) {
            u.nesting.push_back(e.loc);
        }
        break;
    default:
        break;
    }
    Scan(program, e.a, u);
    Scan(program, e.b, u);
    Scan(program, e.c, u);
    bool items = e.kind == ExprKind::Call || e.kind == ExprKind::CallPtr ||
                 e.kind == ExprKind::StructLit || e.kind == ExprKind::Do;
    for (std::uint32_t i = 0; items && i < e.count; i++) {
        Scan(program, program.lists[e.first + i], u);
    }
    for (std::uint32_t i = 0; e.kind == ExprKind::Match && i < e.count; i++) {
        Scan(program, program.clauses[e.first + i].body, u);
    }
}

} // namespace

ExprId CheckInterruptsForm(Checker& ck, const Node& node) {
    const std::string& head = node.items[0].text;
    if (node.items.size() != 1) {
        return Fail(ck, node, head + " takes nothing; write (" + head + ")");
    }
    ExprId id = AddExpr(ck, ExprKind::Interrupts, kVoidType, node);
    ExprAt(ck, id).value = head == "cli" ? 1 : 0;
    return id;
}

void CheckHandler(Checker& ck) {
    Program& program = *ck.out;
    std::int32_t handler = program.handler;
    if (handler < 0) {
        return;
    }
    const Function& h = program.functions[static_cast<std::size_t>(handler)];
    if (program.main < 0) {
        AddError(*ck.errors, h.loc,
                 h.name + " is an interrupt handler, which the startup installs before main; "
                          "write a main");
        return;
    }
    std::vector<bool> by_handler = Reached(program, handler);
    std::vector<bool> by_main = Reached(program, program.main);
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (by_handler[f] && by_main[f]) {
            const Function& fn = program.functions[f];
            AddError(*ck.errors, fn.loc,
                     fn.name + " runs in the interrupt handler " + h.name +
                         " and in main's code, which an interrupt may stop in the middle of " +
                         fn.name + " (one static frame); give the handler a copy of its own");
        }
    }
    Uses u;
    u.globals.assign(program.vars.size(), false);
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (by_handler[f]) {
            Scan(program, program.functions[f].body, u);
        }
    }
    for (const Loc& loc : u.nesting) {
        AddError(*ck.errors, loc,
                 "(cli) where the interrupt handler runs would let interrupts nest; leave it out "
                 "(the handler's return turns them on again)");
    }
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        Var& var = program.vars[v];
        bool global = var.kind == VarKind::Global;
        bool reached = (u.pointers || u.machine) && var.address_taken;
        if ((global && (u.globals[v] || u.machine)) || reached) {
            var.irq_shared = true;
        }
    }
}

} // namespace tisple
