#include "backend/io_bases.hpp"

#include "backend/analysis.hpp"
#include "backend/irq.hpp"

#include <algorithm>
#include <map>

namespace tisple {

namespace {

// the var a place is when it is just a var, else -1
std::int32_t PlainVar(const Program& program, ExprId id) {
    const Expr& e = ExprOf(program, id);
    return e.kind == ExprKind::VarRef ? e.ref : -1;
}

// addr as var plus a constant, for vars cand allows
bool BasePlusConstant(const Program& program, const std::vector<bool>& cand, ExprId addr,
                      std::int32_t& var, std::int32_t& offset) {
    const Expr& e = ExprOf(program, addr);
    std::int32_t v = PlainVar(program, addr);
    if (v >= 0 && cand[static_cast<std::size_t>(v)]) {
        var = v;
        offset = 0;
        return true;
    }
    if (e.kind != ExprKind::Binary || (e.op != Op::Add && e.op != Op::Sub)) {
        return false;
    }
    std::uint16_t k = 0;
    std::int32_t left = PlainVar(program, e.a);
    if (left >= 0 && cand[static_cast<std::size_t>(left)] && ConstValue(program, e.b, k)) {
        var = left;
        offset = e.op == Op::Add ? k : (0x10000 - k) & 0xFFFF;
        return true;
    }
    std::int32_t right = PlainVar(program, e.b);
    if (e.op == Op::Add && right >= 0 && cand[static_cast<std::size_t>(right)] &&
        ConstValue(program, e.a, k)) {
        var = right;
        offset = k;
        return true;
    }
    return false;
}

// every expression under id once for each stamp (nodes may be shared), children first
template <typename Fn>
void Walk(const Program& program, ExprId id, std::vector<std::uint32_t>& seen, std::uint32_t stamp,
          Fn fn) {
    if (id < 0) {
        return;
    }
    if (static_cast<std::size_t>(id) >= seen.size()) {
        seen.resize(program.exprs.size(), 0);
    }
    if (seen[static_cast<std::size_t>(id)] == stamp) {
        return;
    }
    seen[static_cast<std::size_t>(id)] = stamp;
    const Expr e = ExprOf(program, id);
    Walk(program, e.a, seen, stamp, fn);
    Walk(program, e.b, seen, stamp, fn);
    Walk(program, e.c, seen, stamp, fn);
    if (e.kind == ExprKind::Call || e.kind == ExprKind::StructLit || e.kind == ExprKind::Do) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            Walk(program, program.lists[e.first + i], seen, stamp, fn);
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            Walk(program, program.clauses[e.first + i].body, seen, stamp, fn);
        }
    }
    fn(id);
}

bool Candidate(const Program& program, const Var& var) {
    return var.kind == VarKind::Global && KindOf(program, var.type) == TypeKind::U16 &&
           !var.address_taken && !var.placement.has_address && !var.placement.in_aux &&
           !var.placement.reserve;
}

std::vector<bool> FindBases(const Program& program) {
    std::size_t count = program.vars.size();
    std::vector<bool> cand(count, false);
    for (std::size_t v = 0; v < count; v++) {
        cand[v] = Candidate(program, program.vars[v]);
    }
    std::vector<bool> used(count, false);
    for (const Expr& e : program.exprs) {
        std::int32_t var = -1;
        std::int32_t offset = 0;
        if ((e.kind == ExprKind::IoRead || e.kind == ExprKind::IoWrite) &&
            BasePlusConstant(program, cand, e.a, var, offset)) {
            used[static_cast<std::size_t>(var)] = true;
        }
    }
    // a set in the handler's code: its patch would run in both main's and the handler's
    std::vector<bool> handler = HandlerFunctions(program);
    std::vector<std::uint32_t> seen(program.exprs.size(), 0);
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (!handler[f]) {
            continue;
        }
        Walk(program, program.functions[f].body, seen, 1, [&](ExprId id) {
            const Expr& e = ExprOf(program, id);
            std::int32_t set = e.kind == ExprKind::Set ? PlainVar(program, e.a) : -1;
            if (set >= 0) {
                cand[static_cast<std::size_t>(set)] = false;
            }
        });
    }
    std::vector<bool> out(count, false);
    for (std::size_t v = 0; v < count; v++) {
        out[v] = cand[v] && used[v];
    }
    return out;
}

ExprId AddNode(Program& program, const Expr& e) {
    program.exprs.push_back(e);
    return static_cast<ExprId>(program.exprs.size() - 1);
}

std::int32_t PatchFunction(Program& program, IoBases& bases, std::int32_t var) {
    std::size_t v = static_cast<std::size_t>(var);
    if (bases.patch[v] >= 0) {
        return bases.patch[v];
    }
    const Var& base = program.vars[v];
    Function fn;
    fn.name = base.name + ".patch";
    fn.module = base.module;
    fn.loc = base.loc;
    fn.ret = kVoidType;
    fn.io_patch = var;
    Expr body;
    body.kind = ExprKind::Do;
    body.type = kVoidType;
    body.loc = base.loc;
    body.first = static_cast<std::uint32_t>(program.lists.size());
    fn.body = AddNode(program, body);
    program.functions.push_back(fn);
    std::int32_t f = static_cast<std::int32_t>(program.functions.size() - 1);
    bases.patch[v] = f;
    bases.patches.push_back(f);
    return f;
}

void AddCallee(Program& program, std::int32_t owner, std::int32_t callee) {
    std::vector<std::int32_t>& callees = program.functions[static_cast<std::size_t>(owner)].callees;
    if (std::find(callees.begin(), callees.end(), callee) == callees.end()) {
        callees.push_back(callee);
    }
}

// (set base v) at id becomes (do (set base v) (base.patch)); the set inside, a new node
ExprId AddPatchCall(Program& program, IoBases& bases, ExprId id) {
    Expr set = program.exprs[static_cast<std::size_t>(id)];
    std::int32_t f = PatchFunction(program, bases, PlainVar(program, set.a));
    ExprId stored = AddNode(program, set);
    Expr call;
    call.kind = ExprKind::Call;
    call.type = kVoidType;
    call.ref = f;
    call.loc = set.loc;
    call.first = static_cast<std::uint32_t>(program.lists.size());
    ExprId patched = AddNode(program, call);
    Expr both;
    both.kind = ExprKind::Do;
    both.type = kVoidType;
    both.loc = set.loc;
    both.first = static_cast<std::uint32_t>(program.lists.size());
    both.count = 2;
    program.lists.push_back(stored);
    program.lists.push_back(patched);
    program.exprs[static_cast<std::size_t>(id)] = both;
    return stored;
}

std::uint16_t InitialValue(const Var& var) {
    std::uint16_t lo = var.init.size() > 0 ? var.init[0] : 0;
    std::uint16_t hi = var.init.size() > 1 ? var.init[1] : 0;
    return static_cast<std::uint16_t>(lo | (hi << 8));
}

struct Site {
    std::int32_t label = -1;
    std::int32_t offset = 0; // K
};

// the accesses through sym in the kept code, each given a label to patch at
std::vector<Site> LabelSites(Backend& be, const std::vector<bool>& kept, std::int32_t sym) {
    std::vector<Site> sites;
    for (std::size_t f = 0; f < be.code.size(); f++) {
        if (!kept[f] || be.program->functions[f].io_patch >= 0) {
            continue;
        }
        std::vector<AsmItem>& code = be.code[f];
        for (std::size_t i = 0; i < code.size(); i++) {
            const AsmItem& item = code[i];
            if (item.kind != ItemKind::Ins || item.value.sym != sym) {
                continue;
            }
            Site site;
            site.offset = item.value.offset & 0xFFFF;
            site.label = NewSym(be, be.as.symbols[static_cast<std::size_t>(sym)].name + ".site",
                                SymRole::Label, static_cast<std::int32_t>(f));
            code.insert(code.begin() + static_cast<std::ptrdiff_t>(i), MakeLabel(site.label));
            sites.push_back(site);
            i++;
        }
    }
    return sites;
}

// base + K into the operands of the sites with that K: low bytes, then high bytes
void PatchOffset(std::vector<AsmItem>& out, std::int32_t base, std::int32_t offset,
                 const std::vector<std::int32_t>& labels) {
    std::int32_t lo = offset & 0xFF;
    std::int32_t hi = (offset >> 8) & 0xFF;
    out.push_back(MakeIns(Mn::LDA, Mode::Abs, SymValue(base, 0)));
    if (lo != 0) {
        out.push_back(MakeIns(Mn::CLC, Mode::Imp));
        out.push_back(MakeIns(Mn::ADC, Mode::Imm, Num(lo)));
    }
    for (std::int32_t label : labels) {
        out.push_back(MakeIns(Mn::STA, Mode::Abs, SymValue(label, 1)));
    }
    out.push_back(MakeIns(Mn::LDA, Mode::Abs, SymValue(base, 1)));
    if (lo != 0) {
        out.push_back(MakeIns(Mn::ADC, Mode::Imm, Num(hi)));
    } else if (hi != 0) {
        out.push_back(MakeIns(Mn::CLC, Mode::Imp));
        out.push_back(MakeIns(Mn::ADC, Mode::Imm, Num(hi)));
    }
    for (std::int32_t label : labels) {
        out.push_back(MakeIns(Mn::STA, Mode::Abs, SymValue(label, 2)));
    }
}

// a set of base, or a call of a function that may set it, or machine code, under id
bool MaySetBase(const Program& program, const Effects& effects, ExprId id, std::int32_t var) {
    if (id < 0) {
        return false;
    }
    const Expr& e = ExprOf(program, id);
    if (e.kind == ExprKind::Set && PlainVar(program, e.a) == var) {
        return true;
    }
    if (e.kind == ExprKind::Call &&
        effects.writes[static_cast<std::size_t>(e.ref)][static_cast<std::size_t>(var)]) {
        return true;
    }
    if (e.kind == ExprKind::Jsr || e.kind == ExprKind::CallPtr) {
        return true;
    }
    if (MaySetBase(program, effects, e.a, var) || MaySetBase(program, effects, e.b, var) ||
        MaySetBase(program, effects, e.c, var)) {
        return true;
    }
    if (e.kind == ExprKind::Call || e.kind == ExprKind::StructLit || e.kind == ExprKind::Do) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (MaySetBase(program, effects, ListItem(program, e, i), var)) {
                return true;
            }
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (MaySetBase(program, effects, program.clauses[e.first + i].body, var)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

IoBases LowerIoBases(Program& program) {
    IoBases bases;
    bases.base = FindBases(program);
    bases.patch.assign(program.vars.size(), -1);
    if (std::find(bases.base.begin(), bases.base.end(), true) == bases.base.end()) {
        return bases;
    }
    std::size_t original = program.functions.size();
    std::vector<std::uint32_t> seen(program.exprs.size(), 0);
    std::vector<bool> lowered(program.exprs.size(), false); // by expr: a set with its patch
    for (std::size_t f = 0; f < original; f++) {
        std::int32_t owner = static_cast<std::int32_t>(f);
        std::vector<ExprId> sets;
        Walk(program, program.functions[f].body, seen, static_cast<std::uint32_t>(f + 1),
             [&](ExprId id) {
                 const Expr& e = ExprOf(program, id);
                 std::int32_t var = e.kind == ExprKind::Set ? PlainVar(program, e.a) : -1;
                 if (var < 0 || !bases.base[static_cast<std::size_t>(var)]) {
                     return;
                 }
                 if (static_cast<std::size_t>(id) < lowered.size() &&
                     lowered[static_cast<std::size_t>(id)]) {
                     AddCallee(program, owner, bases.patch[static_cast<std::size_t>(var)]);
                     return;
                 }
                 sets.push_back(id);
             });
        for (ExprId id : sets) {
            std::int32_t var = PlainVar(program, ExprOf(program, id).a);
            ExprId stored = AddPatchCall(program, bases, id);
            lowered.resize(program.exprs.size(), false);
            lowered[static_cast<std::size_t>(stored)] = true;
            AddCallee(program, owner, bases.patch[static_cast<std::size_t>(var)]);
        }
    }
    return bases;
}

bool IoBaseAddress(const Program& program, const IoBases& bases, const Effects& effects,
                   ExprId addr, ExprId later, std::int32_t& var, std::int32_t& offset) {
    if (bases.base.empty() || !BasePlusConstant(program, bases.base, addr, var, offset)) {
        return false;
    }
    // the address is worked out first: what comes after it must leave the base as it was
    return !MaySetBase(program, effects, later, var);
}

void EmitIoPatches(Backend& be, const std::vector<bool>& kept) {
    const Program& program = *be.program;
    for (std::int32_t f : be.io_bases.patches) {
        if (!kept[static_cast<std::size_t>(f)]) {
            continue;
        }
        std::int32_t var = program.functions[static_cast<std::size_t>(f)].io_patch;
        std::int32_t sym = be.io_sym[static_cast<std::size_t>(var)];
        std::vector<Site> sites = LabelSites(be, kept, sym);
        std::map<std::int32_t, std::vector<std::int32_t>> by_offset;
        for (const Site& site : sites) {
            by_offset[site.offset].push_back(site.label);
        }
        std::vector<AsmItem>& out = be.code[static_cast<std::size_t>(f)];
        out.clear();
        out.push_back(MakeLabel(be.fn_sym[static_cast<std::size_t>(f)]));
        // the handler's accesses never see half an operand
        bool irq = program.handler >= 0;
        if (irq) {
            out.push_back(MakeIns(Mn::PHP, Mode::Imp));
            out.push_back(MakeIns(Mn::SEI, Mode::Imp));
        }
        std::int32_t base = be.var_sym[static_cast<std::size_t>(var)];
        for (const auto& [offset, labels] : by_offset) {
            PatchOffset(out, base, offset, labels);
        }
        if (irq) {
            out.push_back(MakeIns(Mn::PLP, Mode::Imp));
        }
        out.push_back(MakeIns(Mn::RTS, Mode::Imp));
    }
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        if (be.io_sym[v] >= 0) {
            DefineSymbol(be.as, be.io_sym[v], InitialValue(program.vars[v]));
        }
    }
}

} // namespace tisple
