// (wait_cycles n): one assembler item (asm/wait.hpp), whose code depends on where it lands so
// the count is exact at any address. The passes over the items never drop, merge or move it,
// and treat it as keeping every register and all memory but the scratch byte a variable one
// counts in, while the flags change: peephole.cpp, carry.cpp, dead_regs.cpp; dead_stores.cpp
// takes a variable wait's count as read like any other operand of an item it does not know.
//
// - A constant n is exactly n cycles.
// - A variable n is read from memory by the wait itself, with absolute loads whatever page
//   it is on, so the overhead is the same everywhere: kVarWaitOverhead. A value that is not a
//   plain pair of bytes (a sum, an element at a computed index, a register) goes into a
//   temporary first; that copy is part of working n out.

#include "asm/wait.hpp"
#include "backend/gen.hpp"

namespace tisple {

namespace {

// low byte then high byte, one after the other in memory, with no index
bool PlainPair(const Opnd& o) {
    if (o.size != 2 || o.in_a || o.b[0].kind != ByteKind::Mem || o.b[1].kind != ByteKind::Mem) {
        return false;
    }
    const AsmValue& lo = o.b[0].value;
    const AsmValue& hi = o.b[1].value;
    return lo.part == Part::Whole && hi.part == Part::Whole && lo.sym == hi.sym &&
           hi.offset == lo.offset + 1;
}

void EmitWait(Gen& g, const AsmItem& item) {
    g.out->push_back(item);
    g.cost += static_cast<std::uint64_t>(ItemCycles(g.be->as, item)) * LoopWeight(g);
    if (item.value.sym >= 0) {
        g.be->info[static_cast<std::size_t>(item.value.sym)].weight += 2 * LoopWeight(g);
    }
}

} // namespace

void GenWait(Gen& g, ExprId id) {
    const Expr& e = ExprOf(*g.program, id);
    if (e.value != 0) {
        EmitWait(g, MakeWait(e.value));
        return;
    }
    Opnd n = Eval(g, e.a, nullptr);
    if (!PlainPair(n)) {
        Opnd temp = NewTemp(g, 2);
        CopyOpnd(g, n, temp);
        n = temp;
    }
    EmitWait(g, MakeVarWait(n.b[0].value, SymValue(g.be->scratch)));
}

} // namespace tisple
