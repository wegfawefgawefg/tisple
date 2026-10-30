#include "backend/dispatch.hpp"
#include "backend/frames.hpp"
#include "backend/generate.hpp"
#include "backend/io_bases.hpp"
#include "backend/irq.hpp"
#include "backend/link.hpp"
#include "backend/peephole.hpp"
#include "backend/runtime.hpp"
#include "backend/squeeze.hpp"
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
// otherwise. Squeezed, the tables are bytes the first multiply pulls in: only when the
// multiplies save the budget's hot cycles for each of them (80 cycles a multiply at least)
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
    const SizeBudget& budget = be.budget;
    std::uint64_t bytes = kQuarterSquareBytes;
    if (budget.squeeze > 0 && (budget.hot_cycles == 0 || total * 80 < budget.hot_cycles * bytes)) {
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
// --opt-bisect counts across both. Squeezed far enough, nothing is unrolled or unswitched
void Optimize(Program& program, const BuildOptions& options, OptOptions opt, OptReport& report) {
    if (options.squeeze >= kSqueezeRolled) {
        opt.disabled.push_back("unroll");
        opt.disabled.push_back("unswitch");
    }
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

// the link with code in the card's bank 1: the fewest of the cheapest functions that make the
// program fit (more of them only ever free more room, but for what they keep out of
// $D000-$DFFF: a search on the count). be is the unlinked backend; the linked one replaces it
LinkResult LinkBank1(Backend& be, LinkInput in, const Bank1Facts& facts, Compiled& out,
                     std::vector<Diagnostic>& errors, Bank1Plan& chosen) {
    std::size_t most = 0;
    std::uint32_t bytes = 0;
    while (most < facts.order.size()) {
        std::uint32_t size = facts.bytes[static_cast<std::size_t>(facts.order[most])];
        // far branches make code a little bigger in its own stretch
        if (bytes + size + size / 8 > kBank1End - kBank1Start) {
            break;
        }
        bytes += size + size / 8;
        most++;
    }
    const Backend unlinked = be;
    auto attempt = [&](std::size_t count, std::vector<Diagnostic>& errs) {
        Bank1Plan plan = PlanBank1(unlinked, facts, count);
        in.bank1 = &plan;
        be = unlinked;
        LinkResult result = LinkProgram(be, in, true, out, errs);
        chosen = plan;
        return result;
    };
    if (most == 0) {
        return LinkResult::kNoRoom;
    }
    std::vector<Diagnostic> errs;
    LinkResult full = attempt(most, errs);
    if (full != LinkResult::kOk) {
        errors.insert(errors.end(), errs.begin(), errs.end());
        return full;
    }
    std::size_t low = 0; // does not fit
    std::size_t high = most;
    while (high - low > 1) {
        std::size_t mid = low + (high - low) / 2;
        std::vector<Diagnostic> ignored;
        (attempt(mid, ignored) == LinkResult::kOk ? high : low) = mid;
    }
    return attempt(high, errors);
}

// one build at options.squeeze; what it gave up for room in squeezed. With bank1, code may go
// in the language card's bank 1 when the program does not fit without
LinkResult CompileAt(const Program& source, const BuildOptions& options, Compiled& out,
                     std::vector<Diagnostic>& errors, Squeezed& squeezed, bool bank1) {
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
        return LinkResult::kError;
    }
    PrintSizeNotes(be, kept);
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (!kept[f]) {
            DropFunction(be, static_cast<std::int32_t>(f), added[f]);
        }
    }
    EmitIoPatches(be, kept);
    FindUsedRoutines(be);
    std::vector<AsmItem> runtime;
    EmitRuntime(be, runtime);
    EmitDivRoutines(be, runtime);
    std::vector<AsmItem> irq;
    std::vector<AsmItem> irq_banked;
    IrqPlan irq_plan;
    if (program.handler >= 0) {
        EmitIrqEntry(be, kept, runtime, irq_plan, irq, irq_banked);
    }
    FramePlan plan;
    if (!AllocateFrames(be, plan)) {
        return LinkResult::kError;
    }
    squeezed = NoteSqueezed(be, kept, choices, runtime);
    LinkInput in;
    in.options = &options;
    in.plan = &plan;
    in.kept = &kept;
    in.runtime = &runtime;
    in.irq = &irq;
    in.irq_banked = &irq_banked;
    in.irq_plan = irq_plan;
    std::vector<bool> source_tables = FindAddressedConsts(source);
    in.source_tables = &source_tables;
    // the language card only when the program needs the room: link without it first, then
    // with it, then with code in its bank 1
    bool card_allowed = options.use_card && CardAllowed(program, be.shapes);
    bool bank1_allowed = card_allowed && options.use_bank1 && (bank1 || options.bank1_test > 0);
    Backend unlinked;
    if (card_allowed) {
        unlinked = be;
    }
    std::vector<Diagnostic> link_errors;
    LinkResult result = LinkResult::kNoRoom;
    Bank1Plan banks;
    if (bank1_allowed && options.bank1_test > 0) {
        Bank1Facts facts = FindBank1Facts(be, kept, runtime);
        banks = PlanBank1Test(be, facts, options.bank1_test);
        in.bank1 = &banks;
        result = LinkProgram(be, in, true, out, link_errors);
    } else {
        result = LinkProgram(be, in, false, out, link_errors);
        if (result == LinkResult::kNoRoom && card_allowed) {
            be = unlinked;
            link_errors.clear();
            result = LinkProgram(be, in, true, out, link_errors);
        }
        if (result == LinkResult::kNoRoom && bank1_allowed) {
            be = unlinked;
            std::vector<Diagnostic> bank1_errors;
            Bank1Facts facts = FindBank1Facts(be, kept, runtime);
            LinkResult banked = LinkBank1(be, in, facts, out, bank1_errors, banks);
            if (banked != LinkResult::kNoRoom || !bank1_errors.empty()) {
                result = banked;
                link_errors = bank1_errors;
            }
        }
    }
    errors.insert(errors.end(), link_errors.begin(), link_errors.end());
    if (result != LinkResult::kOk) {
        return result;
    }
    for (std::size_t f = 0; f < banks.functions.size() && out.card_ram; f++) {
        if (banks.functions[f]) {
            squeezed.bank1.push_back(static_cast<std::int32_t>(f));
        }
    }
    if (!squeezed.bank1.empty()) {
        squeezed.bank1_bytes = banks.bytes;
        squeezed.crossings = banks.crossings;
        squeezed.cycles += banks.crossings * kStubCycles;
    }
    out.as = std::move(be.as);
    out.opt = report;
    out.warnings = PackedArrayWarnings(source);
    return LinkResult::kOk;
}

// a build that fits with code in bank 1 (banked, at its level of squeeze) against the
// tighter budgets without it: the first that fits, unless the cycles a run, about, already
// pass the bank 1 build's. The faster goes to out, and the note says what was traded
void ChooseBank1OrSqueeze(const Program& source, const BuildOptions& options, const Squeezed& usual,
                          const Squeezed& banked, Compiled& out, std::vector<Diagnostic>& errors) {
    BuildOptions tighter = options;
    while (tighter.squeeze < kMaxSqueeze) {
        tighter.squeeze++;
        Compiled other;
        std::vector<Diagnostic> tried;
        Squeezed squeezed;
        LinkResult result = CompileAt(source, tighter, other, tried, squeezed, false);
        bool slower = squeezed.cycles >= banked.cycles;
        if (result == LinkResult::kOk && !slower) {
            out = std::move(other);
            errors.insert(errors.end(), tried.begin(), tried.end());
            out.warnings.push_back(SqueezeNote(source, usual, squeezed));
            out.warnings.push_back(Bank1Note(source, banked, &squeezed, false));
            return;
        }
        if (result == LinkResult::kOk) {
            if (banked.squeeze != usual.squeeze) {
                out.warnings.push_back(SqueezeNote(source, usual, banked));
            }
            out.warnings.push_back(Bank1Note(source, banked, &squeezed, true));
            return;
        }
        if (slower || result != LinkResult::kNoRoom) {
            break;
        }
    }
    if (banked.squeeze != usual.squeeze) {
        out.warnings.push_back(SqueezeNote(source, usual, banked));
    }
    out.warnings.push_back(Bank1Note(source, banked, nullptr, true));
}

} // namespace

bool CompileProgram(const Program& source, const BuildOptions& options, Compiled& out,
                    std::vector<Diagnostic>& errors) {
    if (!CheckNoInline(source, options.no_inline, errors)) {
        return false;
    }
    // with no room, the size budget tightens a level at a time until the program fits
    Squeezed usual;
    BuildOptions tighter = options;
    for (;;) {
        std::vector<Diagnostic> tried;
        Squeezed squeezed;
        LinkResult result = CompileAt(source, tighter, out, tried, squeezed, true);
        if (tighter.squeeze == options.squeeze) {
            usual = squeezed;
        }
        if (result == LinkResult::kOk && !squeezed.bank1.empty() && options.bank1_test == 0) {
            // bank 1, or a tighter budget without it: the one that runs faster
            ChooseBank1OrSqueeze(source, tighter, usual, squeezed, out, tried);
            errors.insert(errors.end(), tried.begin(), tried.end());
            return true;
        }
        bool again = result == LinkResult::kNoRoom && tighter.squeeze < kMaxSqueeze;
        if (!again) {
            errors.insert(errors.end(), tried.begin(), tried.end());
            if (result == LinkResult::kNoRoom && tighter.squeeze != options.squeeze) {
                AddError(errors, Loc(),
                         "the program does not fit even with the tightest size budget (squeeze " +
                             std::to_string(kMaxSqueeze) +
                             ": no copy or unrolled loop that adds bytes, multiplies by shift "
                             "and add)");
            }
            if (result == LinkResult::kOk && tighter.squeeze != options.squeeze) {
                out.warnings.push_back(SqueezeNote(source, usual, squeezed));
            }
            return result == LinkResult::kOk;
        }
        tighter.squeeze++;
    }
}

} // namespace tisple
