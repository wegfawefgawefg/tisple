#include "backend/gen.hpp"

#include <algorithm>

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

// a match's value read from a table: this many entries at most, and at least this many
// clauses with values (one is a compare)
constexpr std::uint32_t kMaxValueTable = 32;
constexpr std::size_t kMinValueClauses = 2;

// a value match on a byte whose clauses all give numbers: the scrutinee indexes a table of
// them (two, low and high bytes, for a 16-bit value). The table starts at 0 when the smallest
// case is small, and runs to the scrutinee's largest value when that is close, so no compare
// is left; values without a clause give the wildcard's number, or 0. False when it does not
// apply
bool ValueTable(Gen& g, const Expr& e, const Opnd* dst, std::uint32_t result_size) {
    const Program& program = *g.program;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> cases;
    std::uint32_t fallback = 0;
    for (std::uint32_t i = 0; i < e.count; i++) {
        const Clause& clause = program.clauses[e.first + i];
        const Expr& body = ExprOf(program, clause.body);
        if (body.kind != ExprKind::Literal) {
            return false;
        }
        std::uint32_t v = body.value & (result_size == 2 ? 0xFFFF : 0xFF);
        if (clause.wildcard) {
            fallback = v;
            break;
        }
        cases.push_back({clause.value & 0xFFu, v});
    }
    if (cases.size() < kMinValueClauses) {
        return false;
    }
    if (result_size == 2 &&
        (dst == nullptr || dst->b[0].kind != ByteKind::Mem || dst->b[1].kind != ByteKind::Mem)) {
        return false;
    }
    std::uint32_t lo = 0xFF;
    std::uint32_t hi = 0;
    for (const auto& [value, number] : cases) {
        lo = std::min(lo, value);
        hi = std::max(hi, value);
    }
    std::uint32_t first = lo < 8 ? 0 : lo;
    std::uint32_t max = std::min<std::uint32_t>(MaxOf(g, e.a), 0xFF);
    // below first the subtraction wraps: then only a compare keeps it in the table
    bool check = first != 0 || max + 1 > kMaxValueTable;
    std::uint32_t last = check ? hi : std::max(max, hi);
    if (last - first + 1 > kMaxValueTable) {
        return false;
    }
    std::vector<std::uint32_t> numbers(last - first + 1, fallback);
    for (const auto& [value, number] : cases) {
        numbers[value - first] = number;
    }
    EvalA(g, e.a);
    std::int32_t outside = NewLabel(g);
    std::int32_t end = NewLabel(g);
    if (first != 0) {
        Emit(g, Mn::SEC, Mode::Imp);
        Emit(g, Mn::SBC, Mode::Imm, Num(static_cast<std::int32_t>(first)));
    }
    if (check) {
        Emit(g, Mn::CMP, Mode::Imm, Num(static_cast<std::int32_t>(numbers.size())));
        Branch(g, Mn::BCS, outside);
    }
    int r = g.pin[0] < 0 ? 0 : 1;
    Emit(g, r == 0 ? Mn::TAX : Mn::TAY, Mode::Imp);
    Mode indexed = r == 0 ? Mode::AbsX : Mode::AbsY;
    for (std::uint32_t k = 0; k < result_size; k++) {
        std::int32_t table = NewLabel(g);
        g.tables.push_back(MakeLabel(table));
        for (std::uint32_t number : numbers) {
            g.tables.push_back(
                MakeByte(Num(static_cast<std::int32_t>((number >> (8 * k)) & 0xFF))));
        }
        Emit(g, Mn::LDA, indexed, SymValue(table));
        if (result_size == 2) {
            StoreA(g, *dst, static_cast<int>(k));
        }
    }
    if (check) {
        Jump(g, end);
        PlaceLabel(g, outside);
        if (result_size == 2) {
            CopyOpnd(g, ImmOpnd(static_cast<std::uint16_t>(fallback), 2), *dst);
        } else {
            Emit(g, Mn::LDA, Mode::Imm, Num(static_cast<std::int32_t>(fallback)));
        }
    }
    PlaceLabel(g, end);
    return true;
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
    if (value && size == 1 && ValueTable(g, e, dst, result_size)) {
        return;
    }
    std::vector<std::int32_t> labels(e.count);
    std::vector<Case> cases;
    std::int32_t end = NewLabel(g);
    std::int32_t fallback = -1;
    // a statement's clauses that do nothing (and no wildcard) go straight to the end
    std::vector<bool> idle(e.count, false);
    std::uint32_t used = e.count; // clauses after a wildcard never run
    for (std::uint32_t i = 0; i < e.count; i++) {
        const Clause& clause = program.clauses[e.first + i];
        idle[i] = !value && !HasEffects(program, clause.body);
        labels[i] = idle[i] ? end : NewLabel(g);
        if (clause.wildcard) {
            fallback = labels[i];
            used = i + 1;
            break;
        }
        Case c;
        c.value = clause.value;
        c.label = labels[i];
        cases.push_back(c);
    }
    bool has_wildcard = fallback >= 0;
    if (!has_wildcard) {
        fallback = value ? NewLabel(g) : end;
    }
    if (fallback == end) {
        std::vector<Case> doing;
        for (const Case& c : cases) {
            if (c.label != end) {
                doing.push_back(c);
            }
        }
        cases = doing;
    }
    if (cases.empty() && fallback == end) {
        if (HasEffects(program, e.a)) {
            Eval(g, e.a, nullptr);
        }
    } else if (size == 1) {
        EvalA(g, e.a);
        Dispatch8(g, cases, fallback);
    } else {
        Opnd s = Eval(g, e.a, nullptr);
        Dispatch16(g, s, cases, fallback);
    }
    for (std::uint32_t i = 0; i < used; i++) {
        const Clause& clause = program.clauses[e.first + i];
        if (idle[i]) {
            continue;
        }
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
    if (!has_wildcard && fallback != end) {
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
