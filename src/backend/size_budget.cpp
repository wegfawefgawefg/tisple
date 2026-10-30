#include "backend/size_budget.hpp"

#include "backend/gen.hpp"
#include "backend/peephole.hpp"
#include "source_file.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

namespace tisple {

namespace {

constexpr std::int64_t kJsrBytes = 3;
constexpr std::int64_t kStoreBytes = 4;   // an argument byte stored into the callee's frame
constexpr std::uint64_t kCallCycles = 12; // JSR and RTS
constexpr std::uint64_t kStoreCycles = 6; // an argument byte stored into the callee's frame

constexpr std::uint64_t kMaxBenefit = std::uint64_t(1) << 40;

std::uint64_t Times(std::uint64_t a, std::uint64_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    return a > kMaxBenefit / b ? kMaxBenefit : a * b;
}

std::int64_t NoteGrowth(const SizeNote& note) {
    return note.bytes - note.instead;
}

// the cycles a note's copies save in a run of the program, about: a loop's as measured; a
// call's copy against the callee's own code, a JSR and RTS, and the argument stores
std::uint64_t NoteBenefit(const Backend& be, const SizeNote& note) {
    std::int64_t saved = note.saved;
    if (!note.item.first) {
        std::uint64_t own = be.fn_cost[static_cast<std::size_t>(note.item.second)] + kCallCycles +
                            kStoreCycles * note.bound_bytes;
        std::uint64_t jsr = Times(own, note.weight);
        saved =
            jsr > note.cost ? static_cast<std::int64_t>(std::min(jsr - note.cost, kMaxBenefit)) : 0;
    }
    if (saved <= 0) {
        return 0;
    }
    // Gen::cost weighs loops with at most 64 trips: the code's own weight with whole ones
    std::uint64_t per_run = Times(static_cast<std::uint64_t>(saved), note.runs) /
                            std::max<std::uint64_t>(note.weight, 1);
    std::uint64_t runs = be.fn_all_runs[static_cast<std::size_t>(note.function)];
    return Times(per_run, std::max<std::uint64_t>(runs, 1));
}

std::int64_t CodeBytes(const Backend& be, std::size_t f) {
    std::int64_t bytes = 0;
    for (const AsmItem& item : be.code[f]) {
        bytes += ItemBytes(be.as, item);
    }
    return bytes;
}

// one item, all its copies in the binary together
struct Item {
    std::int64_t growth = 0;
    std::uint64_t benefit = 0;
};

bool Kept(const SizeChoices& choices, const SizeItem& key) {
    std::size_t i = static_cast<std::size_t>(key.second);
    return key.first ? choices.keep_loops[i] : choices.keep_calls[i];
}

// saves the budget's hot cycles or more for each byte it adds: it pays for its bytes, and no
// budget counts it or turns it down
bool Hot(const SizeBudget& budget, const Item& item) {
    if (budget.hot_cycles == 0) {
        return item.growth <= 0;
    }
    std::uint64_t bytes = static_cast<std::uint64_t>(std::max<std::int64_t>(item.growth, 0));
    return item.benefit >= Times(budget.hot_cycles, bytes);
}

// cycles saved per byte added: a's fewer than b's (growths positive)
bool Cheaper(const Item& a, const Item& b) {
    long double x = static_cast<long double>(a.benefit) * static_cast<long double>(b.growth);
    long double y = static_cast<long double>(b.benefit) * static_cast<long double>(a.growth);
    return x < y;
}

std::map<SizeItem, Item> GatherItems(const Backend& be, const std::vector<bool>& kept) {
    std::map<SizeItem, Item> items;
    for (const SizeNote& note : be.size_notes) {
        if (!kept[static_cast<std::size_t>(note.function)]) {
            continue;
        }
        Item& item = items[note.item];
        item.growth += NoteGrowth(note);
        item.benefit = std::min(kMaxBenefit, item.benefit + NoteBenefit(be, note));
    }
    // a function copied at every call has no body of its own in the binary
    for (auto& [key, item] : items) {
        if (!key.first && !kept[static_cast<std::size_t>(key.second)]) {
            item.growth -= CodeBytes(be, static_cast<std::size_t>(key.second));
        }
    }
    return items;
}

bool Verbose() {
    const char* env = std::getenv("TISPLE_SIZE_NOTES");
    return env && *env;
}

std::string ItemName(const Program& program, const SizeItem& key) {
    if (!key.first) {
        return "calls of " + program.functions[static_cast<std::size_t>(key.second)].name;
    }
    const Loc& loc = program.exprs[static_cast<std::size_t>(key.second)].loc;
    const SourceFile& file = program.sources.files[loc.file];
    return "loop at " + file.path + ":" + std::to_string(PosFromOffset(file, loc.span.start).line);
}

void PrintItem(const Program& program, const char* what, const SizeItem& key, const Item& item) {
    std::fprintf(stderr, "size: %s%s grows %lld bytes, saves %llu cycles\n", what,
                 ItemName(program, key).c_str(), static_cast<long long>(item.growth),
                 static_cast<unsigned long long>(item.benefit));
}

// one round of the budget: the items it turns down, the ones over a budget that save the
// fewest cycles for their bytes first
struct Trim {
    const Backend* be = nullptr;
    std::map<SizeItem, Item> items;
    SizeChoices* choices = nullptr;
    // pairs of items one of which has copies inside the other's: when one goes, the other
    // changes, so it waits to be measured again
    std::set<std::pair<SizeItem, SizeItem>> nested;
    std::vector<SizeItem> gone; // this round
};

bool Nested(const Trim& t, const SizeItem& a, const SizeItem& b) {
    return t.nested.count({a, b}) > 0 || t.nested.count({b, a}) > 0;
}

// false when it waits for the next round
bool TurnDown(Trim& t, const SizeItem& key, const char* why) {
    if (Kept(*t.choices, key)) {
        return true;
    }
    for (const SizeItem& other : t.gone) {
        if (Nested(t, key, other)) {
            return false;
        }
    }
    std::size_t i = static_cast<std::size_t>(key.second);
    if (key.first) {
        t.choices->keep_loops[i] = true;
    } else {
        t.choices->keep_calls[i] = true;
    }
    t.gone.push_back(key);
    if (Verbose()) {
        PrintItem(*t.be->program, why, key, t.items[key]);
    }
    return true;
}

// the items `allowed` lets go, those that save the fewest cycles for their bytes first
template <typename Fn> std::vector<SizeItem> CheapestFirst(const Trim& t, Fn allowed) {
    std::vector<SizeItem> out;
    for (const auto& [key, item] : t.items) {
        if (item.growth > 0 && !Kept(*t.choices, key) && allowed(key)) {
            out.push_back(key);
        }
    }
    std::stable_sort(out.begin(), out.end(), [&](const SizeItem& a, const SizeItem& b) {
        return Cheaper(t.items.at(a), t.items.at(b));
    });
    return out;
}

// one function's code over its budget: what its outermost copies add, the growth left
std::int64_t TrimFunction(Trim& t, std::int32_t function) {
    std::map<SizeItem, std::int64_t> parts;
    for (const SizeNote& note : t.be->size_notes) {
        if (note.function == function && note.around.empty()) {
            parts[note.item] += NoteGrowth(note);
        }
    }
    std::int64_t growth = 0;
    for (const auto& [key, bytes] : parts) {
        // hot items pay for their bytes: the budgets are for the rest
        growth += Kept(*t.choices, key) || Hot(t.be->budget, t.items.at(key)) ? 0 : bytes;
    }
    auto in_function = [&](const SizeItem& key) {
        return parts.count(key) > 0 && !Hot(t.be->budget, t.items.at(key));
    };
    for (const SizeItem& key : CheapestFirst(t, in_function)) {
        if (growth <= t.be->budget.function) {
            break;
        }
        if (TurnDown(t, key, "over the function budget: ")) {
            growth -= parts[key];
        }
    }
    return growth;
}

} // namespace

SizeBudget BudgetAt(int squeeze) {
    SizeBudget b;
    b.squeeze = std::clamp(squeeze, 0, kMaxSqueeze);
    int k = b.squeeze;
    b.item = kMaxItemGrowth >> std::min(k, 6);
    b.function = kMaxFunctionGrowth >> std::min(k, 8);
    b.program = kMaxProgramGrowth >> k;
    b.code_share = 3 + k;
    b.hot_cycles = k == kMaxSqueeze ? 0 : kHotCycles << k;
    return b;
}

bool TrimGrowth(const Backend& be, const std::vector<bool>& kept, SizeChoices& choices,
                std::vector<SizeItem>& gone) {
    Trim t;
    t.be = &be;
    t.items = GatherItems(be, kept);
    t.choices = &choices;
    for (const SizeNote& note : be.size_notes) {
        for (const SizeItem& outer : note.around) {
            t.nested.insert({outer, note.item});
        }
    }
    // one item
    const SizeBudget& budget = be.budget;
    auto over = [&](const SizeItem& key) {
        return t.items[key].growth > budget.item && !Hot(budget, t.items[key]);
    };
    for (const SizeItem& key : CheapestFirst(t, over)) {
        TurnDown(t, key, "over the item budget: ");
    }
    // one function's code
    std::int64_t total = 0;
    std::int64_t code = 0;
    for (std::size_t f = 0; f < be.code.size(); f++) {
        if (kept[f]) {
            code += CodeBytes(be, f);
            total += TrimFunction(t, static_cast<std::int32_t>(f));
        }
    }
    // the whole program; the bodies of functions copied everywhere are not in it
    for (const auto& [key, item] : t.items) {
        if (!key.first && !kept[static_cast<std::size_t>(key.second)] && !Kept(choices, key) &&
            !Hot(budget, item)) {
            total -= CodeBytes(be, static_cast<std::size_t>(key.second));
        }
    }
    std::int64_t program = budget.program + ((code - total) >> budget.code_share);
    auto cold = [&](const SizeItem& key) { return !Hot(budget, t.items[key]); };
    for (const SizeItem& key : CheapestFirst(t, cold)) {
        if (total <= program) {
            break;
        }
        if (TurnDown(t, key, "over the program budget: ")) {
            total -= t.items[key].growth;
        }
    }
    gone = t.gone;
    return !t.gone.empty();
}

std::vector<bool> CopiesOverBudget(const Backend& be, const std::vector<bool>& kept) {
    // by call: one copy's bytes at most, and the bytes of the JSR it replaces
    std::map<ExprId, std::pair<std::int64_t, std::int64_t>> sites;
    std::map<std::int32_t, std::vector<ExprId>> calls; // by callee
    for (const SizeNote& note : be.size_notes) {
        if (note.item.first || !kept[static_cast<std::size_t>(note.function)]) {
            continue;
        }
        auto [at, fresh] = sites.insert({note.site, {note.bytes, note.instead}});
        if (fresh) {
            calls[note.item.second].push_back(note.site);
        } else {
            at->second.first = std::max(at->second.first, note.bytes);
        }
    }
    std::vector<bool> out(be.program->functions.size(), false);
    for (const auto& [callee, list] : calls) {
        std::int64_t growth = -CodeBytes(be, static_cast<std::size_t>(callee));
        for (ExprId site : list) {
            growth += sites[site].first - sites[site].second;
        }
        out[static_cast<std::size_t>(callee)] = growth > be.budget.item;
        if (Verbose() && growth > be.budget.item) {
            Item item;
            item.growth = growth;
            PrintItem(*be.program, "not copied by the mid-level: ", {false, callee}, item);
        }
    }
    return out;
}

void PrintSizeNotes(const Backend& be, const std::vector<bool>& kept) {
    if (!Verbose()) {
        return;
    }
    std::map<SizeItem, Item> items = GatherItems(be, kept);
    for (const auto& [key, item] : items) {
        PrintItem(*be.program, "", key, item);
    }
}

// ---- noting the copies, as code generation makes them

std::int64_t RawBytes(const Gen& g, std::size_t mark) {
    std::int64_t bytes = 0;
    for (std::size_t i = mark; i < g.out->size(); i++) {
        bytes += ItemBytes(g.be->as, (*g.out)[i]);
    }
    return bytes;
}

std::int64_t CleanBytes(const Gen& g, std::vector<AsmItem> items) {
    // the code after it may read what it stores
    std::vector<StoreScope> scopes = StoreScopes(*g.be, g.function);
    for (StoreScope& scope : scopes) {
        if (scope == StoreScope::Private) {
            scope = StoreScope::Exposed;
        }
    }
    Peephole(g.be->as, items, scopes, FrameSharers(*g.be, g.function));
    std::int64_t bytes = 0;
    for (const AsmItem& item : items) {
        bytes += ItemBytes(g.be->as, item);
    }
    return bytes;
}

std::size_t StartCopyNote(Gen& g, const SizeItem& item) {
    g.open_notes.push_back(item);
    return g.out->size();
}

void EndCallNote(Gen& g, std::size_t start, ExprId site, std::uint32_t bound_bytes,
                 std::uint64_t cost_before, std::uint64_t weight) {
    SizeNote note;
    note.item = g.open_notes.back();
    g.open_notes.pop_back();
    note.site = site;
    note.function = g.function;
    note.instead = kJsrBytes + kStoreBytes * static_cast<std::int64_t>(bound_bytes);
    // the clean-up only matters to a copy bigger than the JSR
    note.bytes = RawBytes(g, start);
    if (note.bytes > note.instead) {
        note.bytes =
            CleanBytes(g, std::vector<AsmItem>(g.out->begin() + static_cast<std::ptrdiff_t>(start),
                                               g.out->end()));
    }
    note.cost = g.cost + g.routine_cost - cost_before;
    note.weight = weight;
    note.runs = RunWeight(g);
    note.bound_bytes = bound_bytes;
    note.around = g.open_notes;
    g.size_notes.push_back(note);
}

// a call's target, when it is a runtime or division routine: its cycles, about
std::uint64_t RoutineCycles(const Backend& be, const AsmValue& target) {
    const std::uint64_t kRuntime[kRuntimeCount] = {120, 350, 200, 550, 250, 600, 150, 40, 120, 50};
    if (target.sym < 0) {
        return 0;
    }
    for (int r = 0; r < kRuntimeCount; r++) {
        if (target.sym == be.rt_sym[r]) {
            return kRuntime[r];
        }
    }
    if (target.sym == be.divu8_ge || target.sym == be.divu16_ge) {
        return target.sym == be.divu8_ge ? 180 : 500;
    }
    for (const DivRoutine& d : be.div_routines) {
        if (target.sym == d.sym) {
            return d.wide ? 150 : 80;
        }
    }
    return 0;
}

std::uint64_t RunWeight(const Gen& g) {
    std::uint64_t w = 1;
    for (int level = 0; level < g.loop_depth; level++) {
        std::size_t at = static_cast<std::size_t>(level);
        w = Times(w, at < g.loop_runs.size() ? g.loop_runs[at] : 4);
    }
    return w;
}

bool KeepsLoop(const Gen& g, ExprId loop) {
    std::size_t i = static_cast<std::size_t>(loop);
    return i < g.be->keep_loops.size() && g.be->keep_loops[i];
}

} // namespace tisple
