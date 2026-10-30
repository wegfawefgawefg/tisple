#include "backend/dispatch.hpp"
#include "backend/frames.hpp"
#include "backend/generate.hpp"
#include "backend/irq.hpp"
#include "backend/link.hpp"
#include "backend/peephole.hpp"
#include "backend/runtime.hpp"
#include "link/image.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace tisple {

namespace {

// a function no kept code calls: no code, no temporaries, its uses not counted when frames
// pick zero page. Its params and locals stay, since inlined copies of it use them
void DropFunction(Backend& be, std::int32_t function, const WeightsAdded& added) {
    std::size_t f = static_cast<std::size_t>(function);
    be.code[f].clear();
    be.multiplies[f].clear();
    be.temp_size[f] = 0;
    be.ptemp_size[f] = 0;
    be.lptr_size[f] = 0;
    for (const auto& [sym, weight] : added) {
        be.info[static_cast<std::size_t>(sym)].weight -= weight;
    }
}

// the quarter-square routines (1 KB of tables, about half the cycles) for every multiply when
// the multiplies of the kept code together may run more than once in a run of the program
// (every copy counted: inlined, unrolled; the tables serve them all), the shift-and-add ones
// otherwise
void UpgradeMultiplies(Backend& be) {
    const Runtime pairs[3][2] = {{Runtime::Mul8, Runtime::Mul8Q},
                                 {Runtime::Mul16, Runtime::Mul16Q},
                                 {Runtime::Mul8x8, Runtime::Mul8x8Q}};
    std::uint64_t total = 0;
    for (const auto& sites : be.multiplies) {
        for (const auto& [id, n] : sites) {
            total = std::min<std::uint64_t>(total + n, std::uint64_t(1) << 32);
        }
    }
    if (total < 2) {
        return;
    }
    for (std::vector<AsmItem>& code : be.code) {
        for (AsmItem& item : code) {
            for (const auto& pair : pairs) {
                if (item.kind == ItemKind::Ins &&
                    item.value.sym == be.rt_sym[static_cast<int>(pair[0])]) {
                    item.value.sym = be.rt_sym[static_cast<int>(pair[1])];
                }
            }
        }
    }
}

// the runtime and division routines the kept code calls
void FindUsedRoutines(Backend& be) {
    UpgradeMultiplies(be);
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
    if (named[static_cast<std::size_t>(be.divu8_ge)]) {
        be.rt_used[static_cast<int>(Runtime::DivU8)] = true;
    }
    if (named[static_cast<std::size_t>(be.divu16_ge)]) {
        be.rt_used[static_cast<int>(Runtime::DivU16)] = true;
    }
    for (const DivRoutine& r : be.div_routines) {
        if (r.needs >= 0 && named[static_cast<std::size_t>(r.sym)]) {
            named[static_cast<std::size_t>(r.needs)] = true;
        }
    }
    std::vector<DivRoutine> used;
    for (const DivRoutine& r : be.div_routines) {
        if (named[static_cast<std::size_t>(r.sym)]) {
            used.push_back(r);
        }
    }
    be.div_routines = used;
}

// the mid-level passes in two runs: up to where they inline (unrolling and unswitching come
// after), then the size budget's measure, then the rest, which settles at once and inlines.
// --opt-bisect counts across both
void Optimize(Program& program, const BuildOptions& options, OptOptions opt, OptReport& report) {
    if (opt.level <= 0) {
        OptimizeProgram(program, opt, report);
        return;
    }
    OptOptions first = opt;
    for (const char* pass : {"inline", "unroll", "unswitch"}) {
        first.disabled.push_back(pass);
    }
    OptReport before;
    OptimizeProgram(program, first, before);
    AddCopiesOverBudget(program, options, opt.no_inline);
    if (opt.bisect >= 0) {
        opt.bisect =
            std::max<std::int64_t>(0, opt.bisect - static_cast<std::int64_t>(before.applied));
    }
    OptimizeProgram(program, opt, report);
    report.applied += before.applied;
    report.total += before.total;
    if (report.last.empty()) {
        report.last = before.last;
    }
}

} // namespace

bool CompileProgram(const Program& source, const BuildOptions& options, Compiled& out,
                    std::vector<Diagnostic>& errors) {
    // the same program with its constant arithmetic folded and the mid-level passes run on
    // it: what the code is made from. The interpreter keeps running the program as written
    Program program = source;
    FoldConstants(program);
    OptReport report;
    OptOptions opt = options.opt;
    opt.no_inline = options.no_inline; // the mid-level inlines too
    opt.keep_bodies = options.keep_all_functions;
    Optimize(program, options, opt, report);
    out = Compiled();
    out.origin = options.origin;
    out.main = program.main;
    Backend be;
    std::vector<WeightsAdded> added;
    std::vector<bool> kept;
    SizeChoices choices;
    if (!GenerateProgram(program, options, kMaxSizeRounds, be, added, kept, choices, errors)) {
        return false;
    }
    PrintSizeNotes(be, kept);
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (!kept[f]) {
            DropFunction(be, static_cast<std::int32_t>(f), added[f]);
        }
    }
    FindUsedRoutines(be);
    std::vector<AsmItem> runtime;
    EmitRuntime(be, runtime);
    EmitDivRoutines(be, runtime);
    std::vector<AsmItem> irq;
    IrqPlan irq_plan;
    if (program.handler >= 0) {
        EmitIrqEntry(be, kept, runtime, irq_plan, irq);
    }
    FramePlan plan;
    if (!AllocateFrames(be, plan)) {
        return false;
    }
    LinkInput in;
    in.options = &options;
    in.plan = &plan;
    in.kept = &kept;
    in.runtime = &runtime;
    in.irq = &irq;
    in.irq_plan = irq_plan;
    std::vector<bool> source_tables = FindNamedTables(source);
    in.source_tables = &source_tables;
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
    out.opt = report;
    out.warnings = PackedArrayWarnings(source);
    return true;
}

} // namespace tisple
