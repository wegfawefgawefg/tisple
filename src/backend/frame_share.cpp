#include "backend/frame_share.hpp"

#include "backend/frame_live.hpp"

#include <algorithm>

namespace tisple {

namespace {

void FindSyms(const Backend& be, FrameFacts& facts) {
    const Program& program = *be.program;
    std::size_t n = be.info.size();
    facts.syms.assign(n, FrameSym());
    facts.candidate.assign(n, false);
    facts.pinned.assign(n, false);
    facts.named.assign(n, false);
    facts.called.assign(n, -1);
    facts.routine.assign(n, false);
    for (std::size_t sym = 0; sym < n; sym++) {
        const SymInfo& info = be.info[sym];
        FrameSym& fs = facts.syms[sym];
        std::size_t owner = static_cast<std::size_t>(info.owner);
        if (info.role == SymRole::Var) {
            const Var& var = program.vars[owner];
            const Placement& place = var.placement;
            if (var.function < 0 || place.has_address) {
                continue;
            }
            fs.owner = var.function;
            fs.size = be.shapes.vars[owner].size;
            fs.param = var.kind == VarKind::Param;
            bool placed = place.in_aux || place.reserve || place.align > 0 || place.boundary > 0;
            facts.candidate[sym] = var.made && var.kind == VarKind::Local && !var.address_taken &&
                                   !var.irq_shared && !placed && fs.size > 0;
            continue;
        }
        if (info.role == SymRole::Temps) {
            fs.size = be.temp_size[owner];
        } else if (info.role == SymRole::PtrTemps) {
            fs.size = be.ptemp_size[owner];
        } else if (info.role == SymRole::LoopPtrs) {
            fs.size = be.lptr_size[owner];
        } else {
            continue;
        }
        fs.owner = info.owner;
        fs.temps = true;
    }
    for (std::size_t f = 0; f < be.fn_sym.size(); f++) {
        facts.called[static_cast<std::size_t>(be.fn_sym[f])] = static_cast<std::int32_t>(f);
    }
    for (int r = 0; r < kRuntimeCount; r++) {
        facts.routine[static_cast<std::size_t>(be.rt_sym[r])] = true;
    }
    facts.routine[static_cast<std::size_t>(be.divu8_ge)] = true;
    facts.routine[static_cast<std::size_t>(be.divu16_ge)] = true;
    for (const DivRoutine& r : be.div_routines) {
        facts.routine[static_cast<std::size_t>(r.sym)] = true;
    }
}

// below[a][b]: b is reached from a through the frame stack's caller lists, so b's frame
// comes after a's
std::vector<std::vector<bool>> FindBelow(const std::vector<std::vector<std::int32_t>>& callers) {
    std::size_t n = callers.size();
    std::vector<std::vector<std::int32_t>> callees(n);
    for (std::size_t f = 0; f < n; f++) {
        for (std::int32_t c : callers[f]) {
            callees[static_cast<std::size_t>(c)].push_back(static_cast<std::int32_t>(f));
        }
    }
    std::vector<std::vector<bool>> below(n, std::vector<bool>(n, false));
    for (std::size_t a = 0; a < n; a++) {
        std::vector<std::int32_t> work(callees[a].begin(), callees[a].end());
        while (!work.empty()) {
            std::size_t b = static_cast<std::size_t>(work.back());
            work.pop_back();
            if (below[a][b]) {
                continue;
            }
            below[a][b] = true;
            work.insert(work.end(), callees[b].begin(), callees[b].end());
        }
    }
    return below;
}

struct Placer {
    const Backend* be = nullptr;
    const FrameFacts* facts = nullptr;
    std::vector<std::vector<bool>> below;
    std::vector<std::int32_t> home; // by sym: the frame a candidate went to, else -1
};

bool Below(const Placer& pl, std::int32_t a, std::int32_t b) {
    return pl.below[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)];
}

// may the frames of a and b share bytes (functions that may run at different times)
bool Overlap(const Placer& pl, std::int32_t a, std::int32_t b) {
    return a >= 0 && b >= 0 && a != b && !Below(pl, a, b) && !Below(pl, b, a);
}

// the frame a sym's bytes are in: a candidate not placed yet counts as its own function's,
// where it may always go
std::int32_t HomeOf(const Placer& pl, std::int32_t sym) {
    std::int32_t home = pl.home[static_cast<std::size_t>(sym)];
    return home >= 0 ? home : pl.facts->syms[static_cast<std::size_t>(sym)].owner;
}

bool Meet(const FrameFacts& facts, std::int32_t a, std::int32_t b) {
    auto found = facts.meets.find(a);
    return found != facts.meets.end() && found->second.count(b) > 0;
}

// may the candidate's bytes be in the frame of the function it was copied from
bool HomeFits(const Placer& pl, std::int32_t sym, std::int32_t home) {
    const FrameFacts& facts = *pl.facts;
    // only code above that frame names it
    for (std::int32_t f : facts.code_of.at(sym)) {
        if (!Below(pl, f, home)) {
            return false;
        }
    }
    // it holds no value across a call of what may overlap that frame
    auto calls = facts.calls.find(sym);
    if (calls != facts.calls.end()) {
        for (std::int32_t callee : calls->second) {
            if (callee < 0 || !Below(pl, home, callee)) {
                return false;
            }
        }
    }
    // and meets no byte of such a frame
    auto meets = facts.meets.find(sym);
    if (meets != facts.meets.end()) {
        for (std::int32_t other : meets->second) {
            if (Overlap(pl, HomeOf(pl, other), home)) {
                return false;
            }
        }
    }
    return true;
}

// a slot of home's frame none of whose vars the candidate meets: the smallest it fits in,
// else the biggest; -1 when there is none
std::int32_t PickSlot(const Placer& pl, const FrameShares& out, std::int32_t sym,
                      std::int32_t home) {
    std::uint32_t size = pl.facts->syms[static_cast<std::size_t>(sym)].size;
    std::int32_t best = -1;
    for (std::size_t k = 0; k < out.slots.size(); k++) {
        const FrameSlot& slot = out.slots[k];
        if (slot.home != home) {
            continue;
        }
        bool meets = false;
        for (std::int32_t member : slot.vars) {
            meets = meets || Meet(*pl.facts, sym, pl.be->var_sym[static_cast<std::size_t>(member)]);
        }
        if (meets) {
            continue;
        }
        if (best < 0) {
            best = static_cast<std::int32_t>(k);
            continue;
        }
        std::uint32_t was = out.slots[static_cast<std::size_t>(best)].size;
        bool fits = slot.size >= size;
        bool was_fits = was >= size;
        bool smaller_fit = fits && (!was_fits || slot.size < was);
        bool bigger_miss = !fits && !was_fits && slot.size > was;
        if (smaller_fit || bigger_miss) {
            best = static_cast<std::int32_t>(k);
        }
    }
    return best;
}

} // namespace

void ShareMadeLocals(const Backend& be, const std::vector<std::vector<std::int32_t>>& callers,
                     FrameShares& out) {
    const Program& program = *be.program;
    out = FrameShares();
    out.slot_of.assign(program.vars.size(), -1);
    out.unnamed.assign(program.vars.size(), false);
    FrameFacts facts;
    FindSyms(be, facts);
    for (std::size_t f = 0; f < be.code.size(); f++) {
        LookAtCode(facts, static_cast<std::int32_t>(f), be.code[f]);
    }
    Placer pl;
    pl.be = &be;
    pl.facts = &facts;
    pl.below = FindBelow(callers);
    pl.home.assign(facts.syms.size(), -1);
    std::vector<std::int32_t> order;
    for (std::size_t sym = 0; sym < facts.syms.size(); sym++) {
        bool var = be.info[sym].role == SymRole::Var && facts.syms[sym].owner >= 0;
        if (var && !facts.named[sym]) {
            out.unnamed[static_cast<std::size_t>(be.info[sym].owner)] = true;
        }
        if (facts.candidate[sym] && facts.named[sym] && !facts.pinned[sym]) {
            order.push_back(static_cast<std::int32_t>(sym));
        }
    }
    // the most used bytes first: they pick their slots
    auto weight = [&](std::int32_t sym) { return be.info[static_cast<std::size_t>(sym)].weight; };
    auto size = [&](std::int32_t sym) { return facts.syms[static_cast<std::size_t>(sym)].size; };
    std::stable_sort(order.begin(), order.end(), [&](std::int32_t x, std::int32_t y) {
        return weight(x) * size(y) > weight(y) * size(x);
    });
    for (std::int32_t sym : order) {
        std::int32_t var = be.info[static_cast<std::size_t>(sym)].owner;
        const Var& v = program.vars[static_cast<std::size_t>(var)];
        std::int32_t home = v.function;
        if (v.copied_from >= 0 && v.copied_from != v.function && HomeFits(pl, sym, v.copied_from)) {
            home = v.copied_from;
        }
        std::int32_t best = PickSlot(pl, out, sym, home);
        if (best < 0) {
            FrameSlot slot;
            slot.home = home;
            out.slots.push_back(slot);
            best = static_cast<std::int32_t>(out.slots.size() - 1);
        }
        FrameSlot& slot = out.slots[static_cast<std::size_t>(best)];
        slot.size = std::max(slot.size, size(sym));
        slot.forced = slot.forced || PointerInZp(be, var);
        slot.weight += weight(sym);
        slot.vars.push_back(var);
        pl.home[static_cast<std::size_t>(sym)] = home;
        out.slot_of[static_cast<std::size_t>(var)] = best;
    }
}

} // namespace tisple
