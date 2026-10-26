#include "backend/gen.hpp"

namespace tisple {

namespace {

// a jump table beats a compare chain (4-5 cycles a clause) from about this many clauses
constexpr std::uint32_t kMinTableClauses = 5;

struct Case {
    std::uint16_t value = 0;
    std::int32_t label = -1;
};

// push target-1 and RTS into it: A holds the scrutinee
void JumpTable(Gen& g, const std::vector<Case>& cases, std::uint32_t lo, std::uint32_t span,
               std::int32_t fallback) {
    if (lo != 0) {
        Emit(g, Mn::SEC, Mode::Imp);
        Emit(g, Mn::SBC, Mode::Imm, Num(static_cast<std::int32_t>(lo)));
    }
    if (span < 256) {
        Emit(g, Mn::CMP, Mode::Imm, Num(static_cast<std::int32_t>(span)));
        Branch(g, Mn::BCS, fallback);
    }
    int r = g.pin[0] < 0 ? 0 : 1;
    Emit(g, r == 0 ? Mn::TAX : Mn::TAY, Mode::Imp);
    std::int32_t high = NewLabel(g);
    std::int32_t low = NewLabel(g);
    Mode indexed = r == 0 ? Mode::AbsX : Mode::AbsY;
    Emit(g, Mn::LDA, indexed, SymValue(high));
    Emit(g, Mn::PHA, Mode::Imp);
    Emit(g, Mn::LDA, indexed, SymValue(low));
    Emit(g, Mn::PHA, Mode::Imp);
    Emit(g, Mn::RTS, Mode::Imp);
    std::vector<std::int32_t> targets(span, fallback);
    for (const Case& c : cases) {
        targets[c.value - lo] = c.label;
    }
    PlaceLabel(g, high);
    for (std::int32_t target : targets) {
        g.out->push_back(MakeByte(SymValue(target, -1, Part::High)));
    }
    PlaceLabel(g, low);
    for (std::int32_t target : targets) {
        g.out->push_back(MakeByte(SymValue(target, -1, Part::Low)));
    }
}

void Dispatch8(Gen& g, const std::vector<Case>& cases, std::int32_t fallback) {
    if (cases.empty()) {
        Jump(g, fallback);
        return;
    }
    std::uint32_t lo = 0xFF;
    std::uint32_t hi = 0;
    for (const Case& c : cases) {
        lo = c.value < lo ? c.value : lo;
        hi = c.value > hi ? c.value : hi;
    }
    std::uint32_t span = hi - lo + 1;
    std::uint32_t n = static_cast<std::uint32_t>(cases.size());
    if (n >= kMinTableClauses && span <= 3 * n) {
        JumpTable(g, cases, lo, span, fallback);
        return;
    }
    for (const Case& c : cases) {
        Emit(g, Mn::CMP, Mode::Imm, Num(c.value));
        Branch(g, Mn::BEQ, c.label);
    }
    Jump(g, fallback);
}

void Dispatch16(Gen& g, const Opnd& s, const std::vector<Case>& cases, std::int32_t fallback) {
    for (const Case& c : cases) {
        std::int32_t next = NewLabel(g);
        LoadA(g, s, 0);
        Emit(g, Mn::CMP, Mode::Imm, Num(c.value & 0xFF));
        Branch(g, Mn::BNE, next);
        LoadA(g, s, 1);
        Emit(g, Mn::CMP, Mode::Imm, Num(c.value >> 8));
        Branch(g, Mn::BEQ, c.label);
        PlaceLabel(g, next);
    }
    Jump(g, fallback);
}

} // namespace

void GenMatch(Gen& g, const Expr& e, const Opnd* dst, bool value) {
    const Program& program = *g.program;
    std::uint32_t size = ExprSize(program, e.a);
    std::uint32_t result_size = value ? TypeOf(program, e.type).size : 0;
    std::vector<std::int32_t> labels(e.count);
    std::vector<Case> cases;
    std::int32_t end = NewLabel(g);
    std::int32_t fallback = -1;
    for (std::uint32_t i = 0; i < e.count; i++) {
        const Clause& clause = program.clauses[e.first + i];
        labels[i] = NewLabel(g);
        if (clause.wildcard) {
            fallback = labels[i];
            break;
        }
        Case c;
        c.value = clause.value;
        c.label = labels[i];
        cases.push_back(c);
    }
    bool has_wildcard = fallback >= 0;
    if (!has_wildcard) {
        fallback = NewLabel(g);
    }
    if (size == 1) {
        EvalA(g, e.a);
        Dispatch8(g, cases, fallback);
    } else {
        Opnd s = Eval(g, e.a, nullptr);
        Dispatch16(g, s, cases, fallback);
    }
    std::uint32_t used = has_wildcard ? static_cast<std::uint32_t>(cases.size()) + 1 : e.count;
    for (std::uint32_t i = 0; i < used; i++) {
        const Clause& clause = program.clauses[e.first + i];
        PlaceLabel(g, labels[i]);
        TempMark mark = MarkTemps(g);
        if (!value) {
            GenStmt(g, clause.body);
        } else if (result_size == 1) {
            EvalA(g, clause.body);
        } else {
            EvalInto(g, clause.body, *dst);
        }
        ReleaseTemps(g, mark);
        bool last = i + 1 == used && has_wildcard;
        if (!last && ExprOf(program, clause.body).type != kNeverType) {
            Jump(g, end);
        }
    }
    if (!has_wildcard) {
        // nothing matched: the interpreter gives 0
        PlaceLabel(g, fallback);
        if (value && result_size == 1) {
            Emit(g, Mn::LDA, Mode::Imm, Num(0));
        } else if (value) {
            CopyOpnd(g, ImmOpnd(0, 2), *dst);
        }
    }
    PlaceLabel(g, end);
}

} // namespace tisple
