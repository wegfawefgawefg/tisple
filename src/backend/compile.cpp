#include "backend/frames.hpp"
#include "backend/link.hpp"
#include "backend/peephole.hpp"
#include "backend/runtime.hpp"
#include "link/image.hpp"

#include <algorithm>
#include <cstdio>

namespace tisple {

namespace {

// a variable's bytes, as the dead-store pass may see them from this function's code: its own
// locals and parameters and every function's locals are read only by code that writes them
// first (a callee's locals by its inlined copies, or by itself); globals and other
// functions' parameters by any code after a call or the return
StoreScope VarScope(const Var& var, std::int32_t function) {
    if (var.address_taken || var.placement.has_address) {
        return StoreScope::Untracked;
    }
    if (var.kind == VarKind::Local || (var.kind == VarKind::Param && var.function == function)) {
        return StoreScope::Private;
    }
    if (var.kind == VarKind::Global || var.kind == VarKind::Param) {
        return StoreScope::Exposed;
    }
    return StoreScope::Untracked;
}

// by symbol, for this function's code
std::vector<StoreScope> StoreScopes(const Backend& be, std::int32_t function) {
    std::vector<StoreScope> out(be.info.size(), StoreScope::Untracked);
    for (std::size_t s = 0; s < be.info.size(); s++) {
        const SymInfo& info = be.info[s];
        if (info.role == SymRole::Var) {
            out[s] = VarScope(be.program->vars[static_cast<std::size_t>(info.owner)], function);
            continue;
        }
        bool frame = info.role == SymRole::Temps || info.role == SymRole::PtrTemps ||
                     info.role == SymRole::LoopPtrs;
        if (frame && info.owner == function) {
            out[s] = StoreScope::Private;
        }
    }
    return out;
}

void MakeSymbols(Backend& be) {
    const Program& program = *be.program;
    be.var_sym.resize(program.vars.size());
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        std::string name = var.name;
        if (var.function >= 0) {
            name = program.functions[static_cast<std::size_t>(var.function)].name + "." + name;
        }
        std::int32_t sym = NewSym(be, name, SymRole::Var, static_cast<std::int32_t>(v));
        be.var_sym[v] = sym;
        AsmSymbol& s = be.as.symbols[static_cast<std::size_t>(sym)];
        if (var.placement.has_address) {
            DefineSymbol(be.as, sym, var.placement.address);
            s.zp = var.placement.address + be.shapes.vars[v].size <= 0x100;
        } else if (PointerInZp(be, static_cast<std::int32_t>(v))) {
            s.zp = true;
        }
    }
    std::size_t fn_count = program.functions.size();
    be.fn_sym.resize(fn_count);
    be.temp_sym.resize(fn_count);
    be.ptemp_sym.resize(fn_count);
    be.temp_size.assign(fn_count, 0);
    be.ptemp_size.assign(fn_count, 0);
    be.lptr_sym.resize(fn_count);
    be.lptr_size.assign(fn_count, 0);
    be.code.resize(fn_count);
    for (std::size_t f = 0; f < fn_count; f++) {
        const std::string& name = program.functions[f].name;
        std::int32_t owner = static_cast<std::int32_t>(f);
        be.fn_sym[f] = NewSym(be, name, SymRole::Label, owner);
        be.temp_sym[f] = NewSym(be, name + ".temps", SymRole::Temps, owner);
        be.ptemp_sym[f] = NewSym(be, name + ".ptrs", SymRole::PtrTemps, owner);
        be.as.symbols[static_cast<std::size_t>(be.ptemp_sym[f])].zp = true;
        be.lptr_sym[f] = NewSym(be, name + ".loop_ptrs", SymRole::LoopPtrs, owner);
        be.as.symbols[static_cast<std::size_t>(be.lptr_sym[f])].zp = true;
    }
    be.scratch = NewSym(be, "scratch", SymRole::Scratch, -1);
    be.rt_a = NewSym(be, "rt_a", SymRole::Scratch, -1);
    be.rt_b = NewSym(be, "rt_b", SymRole::Scratch, -1);
    be.rt_r = NewSym(be, "rt_r", SymRole::Scratch, -1);
    be.rt_s = NewSym(be, "rt_s", SymRole::Scratch, -1);
    for (std::int32_t s : {be.scratch, be.rt_a, be.rt_b, be.rt_r, be.rt_s}) {
        be.as.symbols[static_cast<std::size_t>(s)].zp = true;
    }
    const char* names[kRuntimeCount] = {"rt.mul8",   "rt.mul16",  "rt.divu8",  "rt.divu16",
                                        "rt.divs8",  "rt.divs16", "rt.mul8x8", "rt.mul8q",
                                        "rt.mul16q", "rt.mul8x8q"};
    for (int r = 0; r < kRuntimeCount; r++) {
        be.rt_sym[r] = NewSym(be, names[r], SymRole::Label, -1);
    }
    be.sq_tables = NewSym(be, "sq_tables", SymRole::Data, -1);
}

// what one function's code added to the symbols' weights
using WeightsAdded = std::vector<std::pair<std::int32_t, std::uint64_t>>;

std::vector<std::uint64_t> Weights(const Backend& be) {
    std::vector<std::uint64_t> out(be.info.size());
    for (std::size_t s = 0; s < be.info.size(); s++) {
        out[s] = be.info[s].weight;
    }
    return out;
}

WeightsAdded AddedSince(const Backend& be, const std::vector<std::uint64_t>& before) {
    WeightsAdded out;
    for (std::size_t s = 0; s < be.info.size(); s++) {
        std::uint64_t old = s < before.size() ? before[s] : 0;
        if (be.info[s].weight != old) {
            out.push_back({static_cast<std::int32_t>(s), be.info[s].weight - old});
        }
    }
    return out;
}

// the function whose entry label this symbol is, or -1
std::int32_t FunctionOfSym(const Backend& be, std::int32_t sym) {
    if (sym < 0) {
        return -1;
    }
    const SymInfo& info = be.info[static_cast<std::size_t>(sym)];
    if (info.role != SymRole::Label || info.owner < 0) {
        return -1;
    }
    bool entry = be.fn_sym[static_cast<std::size_t>(info.owner)] == sym;
    return entry ? info.owner : -1;
}

// by function: its code is in the binary. Main and every function its code (after
// inlining) names, and theirs; with no main, or when asked, all of them
std::vector<bool> KeptFunctions(const Backend& be, const BuildOptions& options) {
    std::size_t count = be.program->functions.size();
    std::int32_t main = be.program->main;
    if (options.keep_all_functions || main < 0) {
        return std::vector<bool>(count, true);
    }
    std::vector<bool> kept(count, false);
    std::vector<std::int32_t> work = {main};
    kept[static_cast<std::size_t>(main)] = true;
    while (!work.empty()) {
        std::int32_t f = work.back();
        work.pop_back();
        for (const AsmItem& item : be.code[static_cast<std::size_t>(f)]) {
            std::int32_t callee = FunctionOfSym(be, item.value.sym);
            if (callee < 0 || kept[static_cast<std::size_t>(callee)]) {
                continue;
            }
            kept[static_cast<std::size_t>(callee)] = true;
            work.push_back(callee);
        }
    }
    return kept;
}

// a function no kept code calls: no code, no temporaries, its uses not counted when frames
// pick zero page. Its params and locals stay, since inlined copies of it use them
void DropFunction(Backend& be, std::int32_t function, const WeightsAdded& added) {
    std::size_t f = static_cast<std::size_t>(function);
    be.code[f].clear();
    be.temp_size[f] = 0;
    be.ptemp_size[f] = 0;
    be.lptr_size[f] = 0;
    for (const auto& [sym, weight] : added) {
        be.info[static_cast<std::size_t>(sym)].weight -= weight;
    }
}

// the runtime and division routines the kept code calls
void FindUsedRoutines(Backend& be) {
    std::vector<bool> named(be.info.size(), false);
    for (const std::vector<AsmItem>& code : be.code) {
        for (const AsmItem& item : code) {
            if (item.value.sym >= 0) {
                named[static_cast<std::size_t>(item.value.sym)] = true;
            }
        }
    }
    for (int r = 0; r < kRuntimeCount; r++) {
        be.rt_used[r] = named[static_cast<std::size_t>(be.rt_sym[r])];
    }
    std::vector<DivRoutine> used;
    for (const DivRoutine& r : be.div_routines) {
        if (named[static_cast<std::size_t>(r.sym)]) {
            used.push_back(r);
        }
    }
    be.div_routines = used;
}

} // namespace

bool CompileProgram(const Program& source, const BuildOptions& options, Compiled& out,
                    std::vector<Diagnostic>& errors) {
    // the same program with its constant arithmetic folded: what the code is made from
    Program program = source;
    FoldConstants(program);
    out = Compiled();
    out.origin = options.origin;
    out.main = program.main;
    std::size_t before = errors.size();
    Backend be;
    be.program = &program;
    be.errors = &errors;
    if (!ShapeProgram(program, be.shapes, errors)) {
        return false;
    }
    be.deref = FindDerefPointers(program);
    be.aliases = FindIndexAliases(program);
    be.unit_steps = FindUnitSteps(program);
    be.effects = ComputeEffects(program);
    be.div_pairs = FindDivPairs(program);
    be.ranges = FindValueRanges(program);
    be.fn_exprs.assign(program.functions.size(), 0);
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        be.fn_exprs[f] = CountExprs(program, program.functions[f].body);
    }
    MakeSymbols(be);
    std::vector<WeightsAdded> added(program.functions.size());
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        std::vector<std::uint64_t> weights = Weights(be);
        GenFunction(be, static_cast<std::int32_t>(f));
        Peephole(be.as, be.code[f], StoreScopes(be, static_cast<std::int32_t>(f)));
        added[f] = AddedSince(be, weights);
    }
    if (errors.size() != before) {
        return false;
    }
    std::vector<bool> kept = KeptFunctions(be, options);
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (!kept[f]) {
            DropFunction(be, static_cast<std::int32_t>(f), added[f]);
        }
    }
    FindUsedRoutines(be);
    std::vector<AsmItem> runtime;
    EmitRuntime(be, runtime);
    EmitDivRoutines(be, runtime);
    FramePlan plan;
    if (!AllocateFrames(be, plan)) {
        return false;
    }
    LinkInput in;
    in.options = &options;
    in.plan = &plan;
    in.kept = &kept;
    in.runtime = &runtime;
    // the language card only when the program needs the room: link without it first
    bool card_allowed = options.use_card && CardAllowed(program, be.shapes);
    Backend unlinked;
    if (card_allowed) {
        unlinked = be;
    }
    std::vector<Diagnostic> link_errors;
    LinkResult result = LinkProgram(be, in, false, out, link_errors);
    if (result == LinkResult::kNoRoom && card_allowed) {
        be = unlinked;
        link_errors.clear();
        result = LinkProgram(be, in, true, out, link_errors);
    }
    errors.insert(errors.end(), link_errors.begin(), link_errors.end());
    if (result != LinkResult::kOk) {
        return false;
    }
    out.as = std::move(be.as);
    return true;
}

} // namespace tisple
