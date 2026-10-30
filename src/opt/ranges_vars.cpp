// Ranges of variables, worked out once at the start of each round of the passes
// (docs/midlevel-optimizer.md, slice 5): what holds everywhere, wherever a pass looks.
//
// - A ranged variable (ranges.hpp: a scalar with no address taken, not hardware) holds only
//   what its writes give it: a global its initial value and what each set stores, a local
//   what its let and sets give, a parameter what every call passes and what its sets give (a
//   function nothing calls: anything). Each value's range is worked out where it is written,
//   inside the loops around it, with the ranges found so far.
// - A step: (let v a) among a do's items, a later item a loop of at most n passes (a for with
//   a literal range, a counting while), and every other write of v a (set v (+ v s)) or
//   (set v (- v s)) in that loop's body, not in a loop inside it. Each pass makes each step at
//   most once, so v lies in a + n * (the steps' ranges, with 0): in the type, or wrapped once
//   as a whole. A step's range in the other signedness is taken when that is narrower: a u16
//   step of 65535 is -1.
// - Three walks, from the types' ranges: what a walk finds holds when the ranges it started
//   from do, so it may stop after any.
// - The same walk notes every set of an array or struct (ranges_elements.cpp).
// - Only functions that may run are looked at: with a main, what it calls, and theirs.
// - Off for a program with jsr or nodes not known here (machine code may write anything).
//   With no main every function may be called from outside, with any arguments, and memory
//   may be written between calls: only locals are followed then.

#include "check/arith.hpp"
#include "opt/collapse.hpp"
#include "opt/licm.hpp"
#include "opt/ranges.hpp"

#include <algorithm>

namespace tisple::opt {

namespace {

constexpr int kWalks = 3;

// one (set v (+ v s)): the loop it is in, the range of s in both signednesses
struct Step {
    ExprId loop = -1;
    Range natural;
    Range other;
};

struct VarInfo {
    bool any = false; // a write nothing is known of
    bool seen = false;
    bool other_seen = false;
    Range natural; // the hull of its writes
    Range other;
    std::uint32_t lets = 0;
    // the step form: the let's do and place in it, its value's range, the steps
    ExprId let_do = -1;
    std::uint32_t let_index = 0;
    Range let_natural;
    bool plain = false; // a set that is no step
    std::vector<Step> steps;
};

// a loop that is an item of a do, and how many passes it makes at most
struct LoopPlace {
    ExprId loop = -1;
    ExprId parent = -1;
    std::uint32_t index = 0;
    std::int64_t trips = -1;
};

struct Walker {
    Pass* p = nullptr;
    Counters counters;
    std::vector<VarInfo> info;
    std::vector<ExprId> loops; // around the node, innermost last
    std::vector<LoopPlace> places;
    ElementStores stores;
};

void Widen(Range& into, bool& seen, Range r) {
    if (!seen) {
        into = r;
        seen = true;
        return;
    }
    into.lo = std::min(into.lo, r.lo);
    into.hi = std::max(into.hi, r.hi);
}

// a value's range in the other signedness: a variable's own other view is narrower than its
// natural range's (65535 or 1 is -1..1 there, not the whole range)
Range OtherOf(const Walker& w, ExprId value, Range natural) {
    const Pass& p = *w.p;
    const Expr& e = At(p, value);
    Range o = OtherView(KindOf(*p.program, e.type), natural);
    if (e.kind != ExprKind::VarRef || !p.ranges.vars ||
        static_cast<std::size_t>(e.ref) >= p.ranges.other.size()) {
        return o;
    }
    Range known = p.ranges.other[static_cast<std::size_t>(e.ref)];
    Range both{std::max(o.lo, known.lo), std::min(o.hi, known.hi)};
    return both.lo <= both.hi ? both : o;
}

// a value written to var, its range worked out here
void Written(Walker& w, std::int32_t var, ExprId value) {
    if (!RangedVar(*w.p, var)) {
        return;
    }
    VarInfo& v = w.info[static_cast<std::size_t>(var)];
    Range r = ValueRange(*w.p, w.counters, value);
    Widen(v.natural, v.seen, r);
    Widen(v.other, v.other_seen, OtherOf(w, value, r));
}

Step StepOf(const Walker& w, ExprId s, bool negate) {
    Step step;
    step.loop = w.loops.empty() ? -1 : w.loops.back();
    step.natural = ValueRange(*w.p, w.counters, s);
    step.other = OtherOf(w, s, step.natural);
    if (negate) {
        step.natural = Range{-step.natural.hi, -step.natural.lo};
        step.other = Range{-step.other.hi, -step.other.lo};
    }
    return step;
}

bool Reads(const Pass& p, ExprId id, std::int32_t var) {
    const Expr& e = At(p, id);
    return e.kind == ExprKind::VarRef && e.ref == var;
}

// (set v (+ v s)), (set v (+ s v)) or (set v (- v s)) of a local: a step
void SetOf(Walker& w, std::int32_t var, ExprId value) {
    const Pass& p = *w.p;
    if (!RangedVar(p, var)) {
        return;
    }
    VarInfo& v = w.info[static_cast<std::size_t>(var)];
    const Expr& e = At(p, value);
    bool add = e.kind == ExprKind::Binary && e.op == Op::Add;
    bool sub = e.kind == ExprKind::Binary && e.op == Op::Sub;
    bool local = VarOf(p, var).kind == VarKind::Local;
    if (local && add && Reads(p, e.a, var) && !Reads(p, e.b, var)) {
        v.steps.push_back(StepOf(w, e.b, false));
    } else if (local && add && Reads(p, e.b, var) && !Reads(p, e.a, var)) {
        v.steps.push_back(StepOf(w, e.a, false));
    } else if (local && sub && Reads(p, e.a, var) && !Reads(p, e.b, var)) {
        v.steps.push_back(StepOf(w, e.b, true));
    } else {
        v.plain = true;
    }
    Written(w, var, value);
}

void Visit(Walker& w, ExprId id, ExprId parent, std::uint32_t index);

void VisitSet(Walker& w, const Expr& e) {
    const Pass& p = *w.p;
    // the place's indexes, then the value
    for (ExprId at = e.a; at >= 0 && At(p, at).kind != ExprKind::VarRef;) {
        const Expr& place = At(p, at);
        if (place.kind == ExprKind::Index) {
            Visit(w, place.b, -1, 0);
        }
        if (place.kind != ExprKind::Index && place.kind != ExprKind::Field) {
            Visit(w, at, -1, 0);
            break;
        }
        at = place.a;
    }
    Visit(w, e.b, -1, 0);
    const Expr& place = At(p, e.a);
    if (place.kind == ExprKind::VarRef) {
        SetOf(w, place.ref, e.b);
        return;
    }
    NoteStore(p, w.counters, e.a, w.stores);
}

void VisitLoop(Walker& w, ExprId id, const Expr& e, ExprId parent, std::uint32_t index) {
    const Pass& p = *w.p;
    ExprId before = parent >= 0 ? CounterBefore(p, parent, index) : -1;
    if (parent >= 0) {
        LoopPlace place;
        place.loop = id;
        place.parent = parent;
        place.index = index;
        place.trips = KnownTrips(p, id, before);
        w.places.push_back(place);
    }
    if (e.kind == ExprKind::For) {
        Visit(w, e.a, -1, 0);
        Visit(w, e.b, -1, 0);
        if (RangedVar(p, e.ref)) {
            w.info[static_cast<std::size_t>(e.ref)].any = true;
        }
    }
    Counter counter;
    bool counted = LoopCounter(p, w.counters, id, before, counter);
    if (counted) {
        w.counters.push_back(counter);
    }
    w.loops.push_back(id);
    if (e.kind == ExprKind::While) {
        Visit(w, e.a, -1, 0);
    }
    Visit(w, e.kind == ExprKind::For ? e.c : e.b, -1, 0);
    w.loops.pop_back();
    if (counted) {
        w.counters.pop_back();
    }
}

void VisitCall(Walker& w, const Expr& e) {
    const Pass& p = *w.p;
    const Function& fn = p.program->functions[static_cast<std::size_t>(e.ref)];
    for (std::uint32_t i = 0; i < e.count; i++) {
        ExprId arg = Item(p, e, i);
        Visit(w, arg, -1, 0);
        if (i < fn.params.size()) {
            w.info[static_cast<std::size_t>(fn.params[i])].plain = true;
            Written(w, fn.params[i], arg);
        }
    }
}

void Visit(Walker& w, ExprId id, ExprId parent, std::uint32_t index) {
    if (id < 0) {
        return;
    }
    const Pass& p = *w.p;
    Expr e = At(p, id);
    switch (e.kind) {
    case ExprKind::Do:
        for (std::uint32_t i = 0; i < e.count; i++) {
            Visit(w, Item(p, e, i), id, i);
        }
        return;
    case ExprKind::Let:
        Visit(w, e.a, -1, 0);
        if (RangedVar(p, e.ref)) {
            VarInfo& v = w.info[static_cast<std::size_t>(e.ref)];
            v.lets++;
            v.let_do = parent;
            v.let_index = index;
            v.let_natural = ValueRange(p, w.counters, e.a);
            Written(w, e.ref, e.a);
        }
        return;
    case ExprKind::Set:
        VisitSet(w, e);
        return;
    case ExprKind::While:
    case ExprKind::For:
        VisitLoop(w, id, e, parent, index);
        return;
    case ExprKind::Call:
        VisitCall(w, e);
        return;
    case ExprKind::Match:
        Visit(w, e.a, -1, 0);
        for (std::uint32_t i = 0; i < e.count; i++) {
            Visit(w, p.program->clauses[e.first + i].body, -1, 0);
        }
        return;
    default:
        EachChild(p, e, [&](ExprId child) { Visit(w, child, -1, 0); });
        return;
    }
}

const LoopPlace* PlaceOf(const Walker& w, ExprId loop) {
    for (const LoopPlace& place : w.places) {
        if (place.loop == loop) {
            return &place;
        }
    }
    return nullptr;
}

// a step local's range (see the top): false when it is not one
bool StepRange(const Walker& w, std::int32_t var, Range& out) {
    const Pass& p = *w.p;
    const VarInfo& v = w.info[static_cast<std::size_t>(var)];
    if (v.any || v.plain || v.steps.empty() || v.let_do < 0 || v.lets != 1) {
        return false;
    }
    ExprId loop = v.steps[0].loop;
    for (const Step& s : v.steps) {
        if (s.loop != loop || loop < 0) {
            return false;
        }
    }
    const LoopPlace* place = PlaceOf(w, loop);
    if (place == nullptr || place->parent != v.let_do || place->index <= v.let_index ||
        place->trips < 0) {
        return false;
    }
    Range type = TypeRange(p, VarOf(p, var).type);
    bool found = false;
    for (int view = 0; view < 2; view++) {
        std::int64_t down = 0;
        std::int64_t up = 0;
        for (const Step& s : v.steps) {
            Range r = view == 0 ? s.natural : s.other;
            down += std::min<std::int64_t>(0, r.lo);
            up += std::max<std::int64_t>(0, r.hi);
        }
        Range m{v.let_natural.lo + place->trips * down, v.let_natural.hi + place->trips * up};
        std::int64_t span = type.hi - type.lo + 1;
        for (std::int64_t shift : {std::int64_t(0), span, -span}) {
            Range moved{m.lo + shift, m.hi + shift};
            bool fits = moved.lo >= type.lo && moved.hi <= type.hi;
            if (fits && (!found || moved.hi - moved.lo < out.hi - out.lo)) {
                out = moved;
                found = true;
            }
        }
    }
    return found;
}

// a global's initial value
Range InitialValue(const Pass& p, std::int32_t var) {
    const Var& v = VarOf(p, var);
    TypeKind kind = KindOf(*p.program, v.type);
    std::uint32_t bits = 0;
    for (std::size_t b = 0; b < v.init.size() && b < 2; b++) {
        bits |= static_cast<std::uint32_t>(v.init[b]) << (8 * b);
    }
    std::int64_t value = SignedValue(kind, MaskToKind(kind, bits));
    return Range{value, value};
}

// what one walk found, as the ranges the next one starts from
void Settle(Walker& w) {
    Pass& p = *w.p;
    VarRanges& r = p.ranges;
    std::vector<bool> called(p.program->functions.size(), false);
    for (const Function& fn : p.program->functions) {
        for (std::int32_t callee : fn.callees) {
            called[static_cast<std::size_t>(callee)] = true;
        }
    }
    for (std::size_t var = 0; var < r.natural.size(); var++) {
        std::int32_t v32 = static_cast<std::int32_t>(var);
        if (!RangedVar(p, v32)) {
            continue;
        }
        const Var& v = p.program->vars[var];
        VarInfo& info = w.info[var];
        Range type = TypeRange(p, v.type);
        TypeKind kind = KindOf(*p.program, v.type);
        if (v.kind == VarKind::Global) {
            Range initial = InitialValue(p, v32);
            Widen(info.natural, info.seen, initial);
            Widen(info.other, info.other_seen, OtherView(kind, initial));
        }
        // a function whose address is taken is called from wherever the pointer goes, with
        // what its calls through it pass: not the calls the tree shows
        bool uncalled = v.kind == VarKind::Param && !called[static_cast<std::size_t>(v.function)];
        uncalled =
            uncalled || (v.kind == VarKind::Param &&
                         p.program->functions[static_cast<std::size_t>(v.function)].address_taken);
        // with no main, called from outside: any argument, any global's bytes between calls
        uncalled = uncalled || (p.program->main < 0 && v.kind != VarKind::Local);
        Range natural = type;
        Range other = OtherView(kind, type);
        Range step;
        if (StepRange(w, v32, step)) {
            natural = step;
            other = OtherView(kind, step);
        } else if (info.seen && !info.any && !uncalled) {
            natural = info.natural;
            other = info.other;
        }
        // what an earlier walk found still holds
        r.natural[var] =
            Range{std::max(r.natural[var].lo, natural.lo), std::min(r.natural[var].hi, natural.hi)};
        r.other[var] =
            Range{std::max(r.other[var].lo, other.lo), std::min(r.other[var].hi, other.hi)};
        if (r.natural[var].lo > r.natural[var].hi || r.other[var].lo > r.other[var].hi) {
            r.natural[var] = type; // none: a walk that saw no write, which cannot be
            r.other[var] = OtherView(kind, type);
        }
    }
}

// by function: may it run? With a main, what main calls and what they call; with none,
// every function (any may be called from outside)
std::vector<bool> Runs(const Program& program) {
    std::vector<bool> runs(program.functions.size(), program.main < 0);
    if (program.main < 0) {
        return runs;
    }
    std::vector<std::int32_t> work = {program.main};
    runs[static_cast<std::size_t>(program.main)] = true;
    while (!work.empty()) {
        std::int32_t f = work.back();
        work.pop_back();
        for (std::int32_t callee : program.functions[static_cast<std::size_t>(f)].callees) {
            if (!runs[static_cast<std::size_t>(callee)]) {
                runs[static_cast<std::size_t>(callee)] = true;
                work.push_back(callee);
            }
        }
    }
    return runs;
}

} // namespace

void ComputeVarRanges(Pass& p, bool vars, bool elements) {
    VarRanges& r = p.ranges;
    r = VarRanges();
    std::vector<bool> runs = Runs(*p.program);
    bool machine = false;
    for (std::size_t f = 0; f < p.summaries.size(); f++) {
        const Summary& s = p.summaries[f];
        machine = machine || (runs[f] && (s.jsr || s.unknown));
    }
    if (machine || (!vars && !elements)) {
        return;
    }
    std::size_t count = p.program->vars.size();
    r.natural.resize(count);
    r.other.resize(count);
    for (std::size_t v = 0; v < count; v++) {
        TypeId type = p.program->vars[v].type;
        r.natural[v] = TypeRange(p, type);
        r.other[v] = OtherView(KindOf(*p.program, type), r.natural[v]);
    }
    r.vars = vars;
    for (int walk = 0; walk < kWalks; walk++) {
        Walker w;
        w.p = &p;
        w.info.assign(count, VarInfo());
        w.stores.stored.assign(count, {});
        w.stores.runs = runs;
        // code that never runs writes nothing
        for (std::size_t f = 0; f < runs.size(); f++) {
            if (runs[f]) {
                Visit(w, p.program->functions[f].body, -1, 0);
            }
        }
        if (vars) {
            Settle(w);
        }
        if (walk + 1 == kWalks || !vars) {
            if (elements) {
                SettleStores(p, w.stores);
                r.elements = true;
            }
            break;
        }
    }
}

} // namespace tisple::opt
