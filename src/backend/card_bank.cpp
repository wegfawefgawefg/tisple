// Code in the language card's bank 1: card_bank.hpp.

#include "backend/card_bank.hpp"

#include "backend/frames.hpp"
#include "backend/irq.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace tisple {

namespace {

constexpr std::int32_t kRuntimeOwner = -2;

std::uint64_t Add(std::uint64_t a, std::uint64_t b) {
    constexpr std::uint64_t kMax = std::uint64_t(1) << 48;
    return std::min(kMax, a + b);
}

// a JSR, a JMP to a label, or a branch: control goes to the operand
bool IsTransfer(const AsmItem& item) {
    if (item.kind != ItemKind::Ins) {
        return false;
    }
    return item.mn == Mn::JSR || (item.mn == Mn::JMP && item.mode == Mode::Abs) ||
           item.mode == Mode::Rel;
}

bool IsPointerMode(Mode mode) {
    return mode == Mode::IndX || mode == Mode::IndY || mode == Mode::Ind;
}

// which piece of code defines each label: a function, or the runtime
std::map<std::int32_t, std::int32_t> LabelOwners(const Backend& be, const std::vector<bool>& kept,
                                                 const std::vector<AsmItem>& runtime) {
    std::map<std::int32_t, std::int32_t> owners;
    for (std::size_t f = 0; f < be.code.size(); f++) {
        if (!kept[f]) {
            continue;
        }
        for (const AsmItem& item : be.code[f]) {
            if (item.kind == ItemKind::Label) {
                owners[item.value.sym] = static_cast<std::int32_t>(f);
            }
        }
    }
    for (const AsmItem& item : runtime) {
        if (item.kind == ItemKind::Label) {
            owners[item.value.sym] = kRuntimeOwner;
        }
    }
    return owners;
}

struct Scan {
    const Backend* be = nullptr;
    std::map<std::int32_t, std::int32_t> owners;
    std::vector<bool> barred; // by function
    Bank1Facts* facts = nullptr;
};

void Bar(Scan& s, std::int32_t owner) {
    if (owner >= 0) {
        s.barred[static_cast<std::size_t>(owner)] = true;
    }
}

// one item of function f's code: what it names, and what that bars
void ScanItem(Scan& s, std::size_t f, const AsmItem& item) {
    const Backend& be = *s.be;
    std::int32_t self = static_cast<std::int32_t>(f);
    if (item.kind == ItemKind::Label || item.kind == ItemKind::Note) {
        return;
    }
    if (item.kind == ItemKind::Ins && IsPointerMode(item.mode)) {
        s.facts->indirect[f] = true;
        if (item.mn == Mn::JMP) {
            Bar(s, self); // to an address only the run knows
        }
    }
    std::int32_t sym = item.value.sym;
    if (sym < 0) {
        bool code = item.kind == ItemKind::Ins && (item.mn == Mn::JSR || item.mn == Mn::JMP);
        std::uint32_t address = static_cast<std::uint32_t>(item.value.offset) & 0xFFFF;
        bool absolute =
            item.kind == ItemKind::Ins && (item.mode == Mode::Abs || item.mode == Mode::AbsX ||
                                           item.mode == Mode::AbsY || item.mode == Mode::Ind);
        if (code || (absolute && address >= kBank1Start && address < kBank1End)) {
            Bar(s, self);
        }
        return;
    }
    const SymInfo& info = be.info[static_cast<std::size_t>(sym)];
    if (info.role == SymRole::Var) {
        s.facts->vars[f].push_back(info.owner);
        return;
    }
    if (info.role == SymRole::Data) {
        Bar(s, self); // the startup's and runtime's own bytes
        return;
    }
    if (info.role != SymRole::Label) {
        return; // temporaries and scratch: zero page or the frames
    }
    auto found = s.owners.find(sym);
    std::int32_t owner = found == s.owners.end() ? -1 : found->second;
    if (owner == self) {
        return;
    }
    bool entry = owner >= 0 && be.fn_sym[static_cast<std::size_t>(owner)] == sym;
    bool runtime_call = owner == kRuntimeOwner;
    if (IsTransfer(item) && (entry || runtime_call)) {
        return; // a call or tail jump: through a stub when it crosses
    }
    if (item.kind == ItemKind::Ins && item.mode == Mode::Imm && !IsTransfer(item)) {
        Bar(s, owner); // its address is a value: it must stay where it is
        return;
    }
    // into another function's code, or reading or writing it
    Bar(s, self);
    Bar(s, owner);
}

// calls between functions per run, the calls of functions copied everywhere (no code of their
// own) handed to their callers
std::vector<std::map<std::int32_t, std::uint64_t>> CodeCalls(const Backend& be,
                                                             const std::vector<bool>& kept) {
    const Program& program = *be.program;
    std::size_t count = program.functions.size();
    auto weights = FindCallWeights(program, be.fn_all_runs, kMaxRunTrips);
    std::vector<std::map<std::int32_t, std::uint64_t>> calls(count);
    for (std::size_t f = 0; f < count; f++) {
        for (const auto& [g, w] : weights[f]) {
            calls[f][g] = Add(calls[f][g], w);
        }
    }
    // callees first: what a copied function calls, its callers call, in their share
    std::vector<std::int32_t> order = CallersFirst(program);
    for (std::size_t i = order.size(); i-- > 0;) {
        std::size_t g = static_cast<std::size_t>(order[i]);
        if (kept[g]) {
            continue;
        }
        std::uint64_t runs = std::max<std::uint64_t>(be.fn_all_runs[g], 1);
        for (std::size_t f = 0; f < count; f++) {
            auto at = calls[f].find(static_cast<std::int32_t>(g));
            if (at == calls[f].end() || f == g) {
                continue;
            }
            long double share = static_cast<long double>(at->second) / runs;
            calls[f].erase(at);
            for (const auto& [h, w] : calls[g]) {
                if (static_cast<std::size_t>(h) != f) {
                    calls[f][h] = Add(calls[f][h], static_cast<std::uint64_t>(w * share));
                }
            }
        }
    }
    return calls;
}

} // namespace

Bank1Facts FindBank1Facts(const Backend& be, const std::vector<bool>& kept,
                          const std::vector<AsmItem>& runtime) {
    const Program& program = *be.program;
    std::size_t count = program.functions.size();
    Bank1Facts facts;
    facts.bytes.assign(count, 0);
    facts.crossings.assign(count, 0);
    facts.vars.assign(count, {});
    facts.indirect.assign(count, false);
    Scan s;
    s.be = &be;
    s.owners = LabelOwners(be, kept, runtime);
    s.barred.assign(count, false);
    s.facts = &facts;
    std::vector<bool> handler = HandlerFunctions(program);
    for (std::size_t f = 0; f < count; f++) {
        const Function& fn = program.functions[f];
        bool entry = std::find(be.entries.begin(), be.entries.end(),
                               static_cast<std::int32_t>(f)) != be.entries.end();
        // code that runs with the aux switches on stays where both memories have it
        bool aux = IsAuxFunction(be.aux, static_cast<std::int32_t>(f));
        if (!kept[f] || handler[f] || fn.dispatcher || fn.io_patch >= 0 || entry || aux) {
            s.barred[f] = true;
        }
        if (!kept[f]) {
            continue;
        }
        for (const AsmItem& item : be.code[f]) {
            facts.bytes[f] += static_cast<std::uint32_t>(ItemBytes(be.as, item));
            ScanItem(s, f, item);
        }
    }
    // runtime code naming a function's labels (it never does): that function stays
    for (const AsmItem& item : runtime) {
        auto found = item.value.sym >= 0 ? s.owners.find(item.value.sym) : s.owners.end();
        if (found != s.owners.end() && found->second >= 0) {
            Bar(s, found->second);
        }
    }
    // crossings if alone in bank 1: its runs (calls in), its calls out, its multiplies
    std::vector<std::map<std::int32_t, std::uint64_t>> calls = CodeCalls(be, kept);
    for (std::size_t f = 0; f < count; f++) {
        if (s.barred[f] || facts.bytes[f] == 0) {
            continue;
        }
        std::uint64_t c = be.fn_all_runs[f];
        for (const auto& [g, w] : calls[f]) {
            c = static_cast<std::size_t>(g) == f ? c : Add(c, w);
        }
        for (const auto& [id, runs] : be.multiplies[f]) {
            c = Add(c, runs);
        }
        facts.crossings[f] = c;
        facts.order.push_back(static_cast<std::int32_t>(f));
    }
    // fewest crossings a byte first; then the bigger
    std::stable_sort(facts.order.begin(), facts.order.end(), [&](std::int32_t a, std::int32_t b) {
        std::size_t x = static_cast<std::size_t>(a);
        std::size_t y = static_cast<std::size_t>(b);
        long double l = static_cast<long double>(facts.crossings[x]) * facts.bytes[y];
        long double r = static_cast<long double>(facts.crossings[y]) * facts.bytes[x];
        return l != r ? l < r : facts.bytes[x] > facts.bytes[y];
    });
    return facts;
}

namespace {

Bank1Plan PlanFor(const Backend& be, const Bank1Facts& facts, const std::vector<bool>& chosen) {
    const Program& program = *be.program;
    Bank1Plan plan;
    plan.functions = chosen;
    plan.avoiders.assign(program.vars.size(), false);
    bool indirect = false;
    for (std::size_t f = 0; f < chosen.size(); f++) {
        if (!chosen[f]) {
            continue;
        }
        plan.bytes += facts.bytes[f];
        plan.crossings = Add(plan.crossings, facts.crossings[f]);
        indirect = indirect || facts.indirect[f];
        for (std::int32_t v : facts.vars[f]) {
            plan.avoiders[static_cast<std::size_t>(v)] = true;
        }
    }
    for (std::size_t v = 0; v < program.vars.size() && indirect; v++) {
        plan.avoiders[v] = plan.avoiders[v] || program.vars[v].address_taken;
    }
    return plan;
}

} // namespace

Bank1Plan PlanBank1(const Backend& be, const Bank1Facts& facts, std::size_t count) {
    std::vector<bool> chosen(be.program->functions.size(), false);
    for (std::size_t i = 0; i < count && i < facts.order.size(); i++) {
        chosen[static_cast<std::size_t>(facts.order[i])] = true;
    }
    return PlanFor(be, facts, chosen);
}

Bank1Plan PlanBank1Test(const Backend& be, const Bank1Facts& facts, int pick) {
    std::vector<bool> chosen(be.program->functions.size(), false);
    std::uint32_t bytes = 0;
    std::uint64_t s = static_cast<std::uint64_t>(pick) * 0x9E3779B97F4A7C15ull + 1;
    for (std::int32_t f : facts.order) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        bool take = pick == 1 || (s >> 20) % 2 == 0;
        // a bank holds 4 KB, its far branches a little more each
        std::uint32_t size = facts.bytes[static_cast<std::size_t>(f)];
        if (take && bytes + size + size / 8 < kBank1End - kBank1Start) {
            chosen[static_cast<std::size_t>(f)] = true;
            bytes += size + size / 8;
        }
    }
    Bank1Plan plan = PlanFor(be, facts, chosen);
    plan.all_stubs = true;
    return plan;
}

void RouteBankCalls(Backend& be, const std::vector<std::vector<AsmItem>*>& pieces,
                    const std::vector<bool>& bank1, const Bank1Plan& plan,
                    std::vector<AsmItem>& stubs, std::vector<std::int32_t>& forward) {
    std::map<std::int32_t, bool> in_bank1; // by label
    for (std::size_t p = 0; p < pieces.size(); p++) {
        for (const AsmItem& item : *pieces[p]) {
            if (item.kind == ItemKind::Label) {
                in_bank1[item.value.sym] = bank1[p];
            }
        }
    }
    // by (target, offset, into bank 1): its stub's label
    std::map<std::pair<std::pair<std::int32_t, std::int32_t>, bool>, std::int32_t> made;
    auto stub = [&](const AsmValue& target, bool into) {
        auto key = std::make_pair(std::make_pair(target.sym, target.offset), into);
        auto found = made.find(key);
        if (found != made.end()) {
            return found->second;
        }
        const std::string& name = be.as.symbols[static_cast<std::size_t>(target.sym)].name;
        std::string offset = target.offset == 0 ? "" : "+" + std::to_string(target.offset);
        std::int32_t label =
            NewSym(be, (into ? "bank1." : "bank2.") + name + offset, SymRole::Label, -1);
        AsmValue go = SymValue(target.sym, target.offset);
        stubs.push_back(MakeLabel(label));
        stubs.push_back(MakeIns(Mn::BIT, Mode::Abs, Num(into ? kBank1Switch : kBank2Switch)));
        stubs.push_back(MakeIns(Mn::JSR, Mode::Abs, go));
        stubs.push_back(MakeIns(Mn::BIT, Mode::Abs, Num(into ? kBank2Switch : kBank1Switch)));
        stubs.push_back(MakeIns(Mn::RTS, Mode::Imp));
        made[key] = label;
        return label;
    };
    forward.assign(be.program->functions.size(), -1);
    for (std::size_t f = 0; f < plan.functions.size(); f++) {
        if (plan.functions[f] && plan.all_stubs) {
            forward[f] = stub(SymValue(be.fn_sym[f]), true);
        }
    }
    for (std::size_t p = 0; p < pieces.size(); p++) {
        for (AsmItem& item : *pieces[p]) {
            if (!IsTransfer(item) || item.value.sym < 0) {
                continue;
            }
            auto found = in_bank1.find(item.value.sym);
            if (found == in_bank1.end() || found->second == bank1[p]) {
                continue;
            }
            item.value = SymValue(stub(item.value, found->second));
        }
    }
    for (std::size_t f = 0; f < plan.functions.size(); f++) {
        auto found = made.find(std::make_pair(std::make_pair(be.fn_sym[f], 0), true));
        if (plan.functions[f] && found != made.end()) {
            forward[f] = found->second;
        }
    }
}

void RouteBank1(Backend& be, const Bank1Plan& plan, const std::vector<bool>& kept,
                std::vector<AsmItem>& startup, std::vector<AsmItem>& runtime,
                std::vector<AsmItem>& irq, std::vector<AsmItem>& stubs,
                std::vector<std::int32_t>& forward) {
    std::vector<std::vector<AsmItem>*> pieces = {&startup, &runtime, &irq};
    std::vector<bool> bank1 = {false, false, false};
    for (std::size_t f = 0; f < be.code.size(); f++) {
        if (kept[f]) {
            pieces.push_back(&be.code[f]);
            bank1.push_back(plan.functions[f]);
        }
    }
    RouteBankCalls(be, pieces, bank1, plan, stubs, forward);
}

} // namespace tisple
