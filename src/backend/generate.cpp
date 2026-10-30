// Every function's code within the size budget (size_budget.hpp): the symbols, a generation
// of every function, then, while the budget turns copies down, the functions that hold them
// made again.

#include "backend/generate.hpp"

#include "backend/dispatch.hpp"
#include "backend/frames.hpp"
#include "backend/io_bases.hpp"
#include "backend/peephole.hpp"

#include <algorithm>

namespace tisple {

namespace {

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
        s.shared = var.irq_shared;
    }
    // an I/O base's accesses: I/O to the passes (shared, at $C000) until EmitIoPatches
    be.io_sym.assign(program.vars.size(), -1);
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        if (be.io_bases.base.empty() || !be.io_bases.base[v]) {
            continue;
        }
        std::int32_t sym = NewSym(be, program.vars[v].name + ".io", SymRole::Data, -1);
        DefineSymbol(be.as, sym, 0xC000);
        be.as.symbols[static_cast<std::size_t>(sym)].shared = true;
        be.io_sym[v] = sym;
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
    be.divu8_ge = NewSym(be, "rt.divu8.ge", SymRole::Label, -1);
    be.divu16_ge = NewSym(be, "rt.divu16.ge", SymRole::Label, -1);
    be.sq_tables = NewSym(be, "sq_tables", SymRole::Data, -1);
}

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

// what the budget's choices say of the calls (they stay JSRs: no copies) and the loops
void UseChoices(Backend& be, const SizeChoices& choices) {
    for (std::size_t f = 0; f < be.program->functions.size(); f++) {
        if (choices.keep_calls[f]) {
            be.inlines[f] = InlineKind::None;
        }
    }
    be.keep_loops = choices.keep_loops;
}

void GenerateOne(Backend& be, std::size_t f, WeightsAdded& added) {
    std::vector<std::uint64_t> weights = Weights(be);
    if (be.program->functions[f].dispatcher) {
        GenDispatcher(be, static_cast<std::int32_t>(f));
    } else if (be.program->functions[f].io_patch >= 0) {
        // made once all code is final (EmitIoPatches)
    } else {
        GenFunction(be, static_cast<std::int32_t>(f));
        Peephole(be.as, be.code[f], StoreScopes(be, static_cast<std::int32_t>(f)),
                 FrameSharers(be, static_cast<std::int32_t>(f)));
    }
    added = AddedSince(be, weights);
}

// every function's code, with the copies the size budget allows (be as prepared: analyses
// done, no symbols yet)
void GenerateFunctions(Backend& be, const SizeChoices& choices, std::vector<WeightsAdded>& added) {
    const Program& program = *be.program;
    UseChoices(be, choices);
    be.deref = FindDerefPointers(program, be.inlines);
    be.fn_cost.assign(program.functions.size(), 0);
    MakeSymbols(be);
    added.assign(program.functions.size(), {});
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        GenerateOne(be, f, added[f]);
    }
}

// the functions that hold copies of what the budget just turned down, made again; the others
// stay as they are. False (nothing done) when the choices change which pointers live in zero
// page (FindDerefPointers), which every function's code depends on
bool RegenerateFunctions(Backend& be, const SizeChoices& choices, const std::vector<SizeItem>& gone,
                         std::vector<WeightsAdded>& added) {
    const Program& program = *be.program;
    std::vector<InlineKind> inlines = be.inlines;
    UseChoices(be, choices);
    if (FindDerefPointers(program, be.inlines) != be.deref) {
        be.inlines = inlines;
        return false;
    }
    std::vector<bool> again(program.functions.size(), false);
    for (const SizeNote& note : be.size_notes) {
        if (std::find(gone.begin(), gone.end(), note.item) != gone.end()) {
            again[static_cast<std::size_t>(note.function)] = true;
        }
    }
    std::vector<SizeNote> notes;
    for (const SizeNote& note : be.size_notes) {
        if (!again[static_cast<std::size_t>(note.function)]) {
            notes.push_back(note);
        }
    }
    be.size_notes = notes;
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (!again[f]) {
            continue;
        }
        for (const auto& [sym, weight] : added[f]) {
            be.info[static_cast<std::size_t>(sym)].weight -= weight;
        }
        be.code[f].clear();
        be.multiplies[f].clear();
        be.temp_size[f] = 0;
        be.ptemp_size[f] = 0;
        be.lptr_size[f] = 0;
        be.fn_cost[f] = 0;
        GenerateOne(be, f, added[f]);
    }
    return true;
}

} // namespace

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
    // the interrupt handler: the IRQ entry calls it
    std::int32_t handler = be.program->handler;
    if (handler >= 0) {
        kept[static_cast<std::size_t>(handler)] = true;
        work.push_back(handler);
    }
    // the entries data holds (a const table, a let's initial value): data, not code that
    // names them
    for (const Var& var : be.program->vars) {
        for (const AddrSlot& slot : var.addr_slots) {
            if (slot.function < 0) {
                continue;
            }
            std::int32_t entry = be.entries[static_cast<std::size_t>(slot.function)];
            if (!kept[static_cast<std::size_t>(entry)]) {
                kept[static_cast<std::size_t>(entry)] = true;
                work.push_back(entry);
            }
        }
    }
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

// the code of a program the mid-level passes are done with, made again with fewer copies
// while the copies go over the size budget (rounds generations at most): in be, with the
// functions it keeps and what each added to the symbols' weights. choices: what the budget
// turned down
bool GenerateProgram(Program& program, const BuildOptions& options, int rounds, Backend& be,
                     std::vector<WeightsAdded>& added, std::vector<bool>& kept,
                     SizeChoices& choices, std::vector<Diagnostic>& errors) {
    std::vector<std::int32_t> entries = LowerIndirectCalls(program);
    IoBases io_bases = LowerIoBases(program);
    std::vector<ExprId> counter_literals = AddCounterLiterals(program);
    std::size_t before = errors.size();
    Backend prepared;
    prepared.program = &program;
    prepared.errors = &errors;
    if (!ShapeProgram(program, prepared.shapes, errors)) {
        return false;
    }
    prepared.counter_literals = counter_literals;
    prepared.entries = entries;
    prepared.io_bases = io_bases;
    prepared.inlines = FindInlines(program, options.no_inline);
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (program.functions[f].io_patch >= 0) {
            prepared.inlines[f] = InlineKind::None;
        }
        if (program.functions[f].dispatcher || program.functions[f].interrupt) {
            prepared.inlines[f] = InlineKind::None; // only for the analyses; the IRQ entry calls it
        }
    }
    prepared.aliases = FindIndexAliases(program);
    prepared.unit_steps = FindUnitSteps(program);
    prepared.effects = ComputeEffects(program);
    prepared.div_pairs = FindDivPairs(program);
    prepared.ranges = FindValueRanges(program);
    prepared.fn_runs = FindRunCounts(program);
    prepared.fn_all_runs = FindRunCounts(program, kMaxRunTrips);
    prepared.multiplies.assign(program.functions.size(), {});
    prepared.budget = BudgetAt(options.squeeze);
    choices.keep_calls.assign(program.functions.size(), false);
    choices.keep_loops.assign(program.exprs.size(), false);
    std::vector<SizeItem> gone;
    for (int round = 1;; round++) {
        if (round == 1 || !RegenerateFunctions(be, choices, gone, added)) {
            be = prepared;
            GenerateFunctions(be, choices, added);
        }
        if (errors.size() != before) {
            return false;
        }
        kept = KeptFunctions(be, options);
        gone.clear();
        if (round >= rounds || !TrimGrowth(be, kept, choices, gone)) {
            return true;
        }
    }
}

// the small functions whose copies at their calls go over the size budget: the mid-level
// copies them nowhere either (its copies are code the budget cannot see). Measured on the
// program as the mid-level passes leave it before they inline, in one generation with every
// copy made
void AddCopiesOverBudget(const Program& settled, const BuildOptions& options,
                         std::vector<std::string>& no_inline) {
    Program program = settled;
    Backend be;
    std::vector<WeightsAdded> added;
    std::vector<bool> kept;
    SizeChoices choices;
    std::vector<Diagnostic> errors;
    if (!GenerateProgram(program, options, 1, be, added, kept, choices, errors)) {
        return;
    }
    std::vector<bool> over = CopiesOverBudget(be, kept);
    for (std::size_t f = 0; f < settled.functions.size(); f++) {
        if (over[f]) {
            no_inline.push_back(settled.functions[f].name);
        }
    }
}

} // namespace tisple
