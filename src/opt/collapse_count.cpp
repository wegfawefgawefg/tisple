// How many passes a for makes when its bounds are not literals, for loop collapse's
// accumulators (collapse.cpp): behind its (if (< a b) ...) guard the for makes b - a passes,
// worked out into a u16 local first, from copies of the bounds:
//   (for i u8 3 (& g 7) (set x (+ x 1)))
//   -> (if (< 3 (& g 7)) (do (let n u16 (as u16 (- (as i16 (& g 7)) (as i16 3))))
//          (set x (+ x n))))
// Byte counters subtract as i16 and words as u16, so b - a is exact. Only when the bounds are
// pure, read no hardware, and read nothing the loop writes (the copies are worked out where
// the loop was, the bounds once as it starts: they must agree).

#include "opt/collapse.hpp"
#include "opt/licm.hpp"
#include "opt/ranges.hpp"

namespace tisple::opt {

namespace {

bool ReadsWritten(const Pass& p, ExprId id, const LoopWrites& w) {
    if (id < 0) {
        return false;
    }
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::VarRef && w.vars[static_cast<std::size_t>(e.ref)]) {
        return true;
    }
    bool reads = false;
    EachChild(p, e, [&](ExprId child) { reads = reads || ReadsWritten(p, child, w); });
    return reads;
}

ExprId NewConvert(Pass& p, TypeId to, ExprId from, const Loc& loc) {
    if (At(p, from).type == to) {
        return from;
    }
    Expr e;
    e.kind = ExprKind::Convert;
    e.type = to;
    e.a = from;
    e.loc = loc;
    return NewExpr(p, e);
}

} // namespace

bool NeedsCount(const BodyPlan& plan) {
    for (const Accumulator& acc : plan.accumulators) {
        if (acc.op != Op::Or && acc.op != Op::And) {
            return true;
        }
    }
    return false;
}

// + and - add up over a count worked out at run time; ^ needs to know it is odd
bool CountWillDo(const BodyPlan& plan) {
    for (const Accumulator& acc : plan.accumulators) {
        if (acc.op == Op::Xor) {
            return false;
        }
    }
    return true;
}

bool CountableBounds(const Pass& p, ExprId loop) {
    const Expr& e = At(p, loop);
    if (e.kind != ExprKind::For || e.b < 0 || !IsPure(p, e.a) || !IsPure(p, e.b) ||
        !Quiet(p, e.a) || !Quiet(p, e.b)) {
        return false;
    }
    LoopWrites w = FindLoopWrites(p, loop);
    return !w.all && !ReadsWritten(p, e.a, w) && !ReadsWritten(p, e.b, w);
}

ExprId CountLet(Pass& p, std::int32_t function, ExprId loop, std::int32_t& var) {
    Expr e = At(p, loop);
    TypeKind kind = KindOf(*p.program, At(p, e.a).type);
    TypeId wide = KindBits(kind) == 8 ? kI16Type : kU16Type;
    ExprId a = NewConvert(p, wide, CloneExpr(p, e.a), e.loc);
    ExprId b = NewConvert(p, wide, CloneExpr(p, e.b), e.loc);
    Expr sub;
    sub.kind = ExprKind::Binary;
    sub.op = Op::Sub;
    sub.type = wide;
    sub.a = b;
    sub.b = a;
    sub.loc = e.loc;
    ExprId count = NewConvert(p, kU16Type, NewExpr(p, sub), e.loc);
    var = NewLocal(p, function, kU16Type, e.loc);
    Expr let;
    let.kind = ExprKind::Let;
    let.type = kVoidType;
    let.ref = var;
    let.a = count;
    let.loc = e.loc;
    return NewExpr(p, let);
}

} // namespace tisple::opt
