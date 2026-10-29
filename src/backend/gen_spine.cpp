#include "backend/gen.hpp"

#include <algorithm>

// A 16-bit run of + - & | ^ in which every operation's other operand is read where it is,
// with one + or - at most: (& (- i k) m), (| (+ a b) c). It is worked out a byte at a time
// through A, low byte first, so nothing goes to a temporary between the operations. The add's
// carry goes from the low byte to the high one across the logic operations, which leave C
// alone; a second add would need a second carry, so it is not done this way.

namespace tisple {

namespace {

// operations along the run this many deep at most
constexpr int kMaxSpine = 8;

struct SpineStep {
    Op op = Op::Add;
    Opnd o;
};

bool SpineOp(Op op) {
    return op == Op::Add || op == Op::Sub || op == Op::And || op == Op::Or || op == Op::Xor;
}

bool Commuting(Op op) {
    return op != Op::Sub;
}

bool Readable(const Opnd& o) {
    return o.size == 2 && !o.in_a;
}

// byte k of o leaves A as it is under op
bool Identity(const Gen& g, Op op, const Opnd& o, int k) {
    int v = KnownImm(g, o, k);
    return (v == 0 && (op == Op::Or || op == Op::Xor)) || (v == 0xFF && op == Op::And);
}

Mn SpineMn(Op op) {
    switch (op) {
    case Op::Add:
        return Mn::ADC;
    case Op::Sub:
        return Mn::SBC;
    case Op::And:
        return Mn::AND;
    case Op::Or:
        return Mn::ORA;
    default:
        return Mn::EOR;
    }
}

} // namespace

bool EvalSpine16(Gen& g, ExprId id, const Opnd* dst, Opnd& out) {
    const Program& program = *g.program;
    if (HasEffects(program, id)) {
        return false;
    }
    std::vector<SpineStep> steps;
    int adds = 0;
    ExprId at = id;
    Opnd base;
    for (int depth = 0;; depth++) {
        const Expr& x = ExprOf(program, at);
        bool spine = x.kind == ExprKind::Binary && SpineOp(x.op) && ExprSize(program, at) == 2;
        if (!spine || depth == kMaxSpine) {
            base = TrySimple(g, at);
            break;
        }
        SpineStep step;
        step.op = x.op;
        step.o = TrySimple(g, x.b);
        ExprId next = x.a;
        if (!Readable(step.o) && Commuting(x.op)) {
            step.o = TrySimple(g, x.a);
            next = x.b;
        }
        if (!Readable(step.o)) {
            return false;
        }
        adds += x.op == Op::Add || x.op == Op::Sub ? 1 : 0;
        steps.push_back(step);
        at = next;
    }
    if (steps.size() < 2 || adds > 1 || !Readable(base)) {
        return false;
    }
    std::reverse(steps.begin(), steps.end());
    Opnd d = dst ? *dst : NewTemp(g, 2);
    bool crossed = CrossAliases(g, base, d);
    for (const SpineStep& s : steps) {
        crossed = crossed || CrossAliases(g, s.o, d);
    }
    Opnd into = crossed ? NewTemp(g, 2) : d;
    for (int k = 0; k < 2; k++) {
        LoadA(g, base, k);
        for (const SpineStep& s : steps) {
            bool add = s.op == Op::Add || s.op == Op::Sub;
            if (!add && Identity(g, s.op, s.o, k)) {
                continue;
            }
            if (add && k == 0) {
                Emit(g, s.op == Op::Add ? Mn::CLC : Mn::SEC, Mode::Imp);
            }
            OpA(g, SpineMn(s.op), s.o, k);
        }
        StoreA(g, into, k);
    }
    if (crossed) {
        CopyOpnd(g, into, d);
    }
    out = d;
    return true;
}

} // namespace tisple
