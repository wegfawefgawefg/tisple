#include "superopt/stochastic.hpp"

#include <bit>
#include <cmath>

namespace tisple {

namespace {

constexpr double kTemperature = 2.0;
constexpr int kWrongWeight = 8;

int Popcount(unsigned v) {
    return std::popcount(v);
}

// bits of live state that differ from the target
int WrongBits(const Window& w, const SoState& x, const SoState& t) {
    if (x.bad != 0) {
        return 64;
    }
    int wrong = 0;
    if ((w.live & kLocA) != 0) {
        wrong += Popcount(static_cast<unsigned>(x.a ^ t.a));
    }
    if ((w.live & kLocX) != 0) {
        wrong += Popcount(static_cast<unsigned>(x.x ^ t.x));
    }
    if ((w.live & kLocY) != 0) {
        wrong += Popcount(static_cast<unsigned>(x.y ^ t.y));
    }
    unsigned flags = static_cast<unsigned>(
        ((w.live & kLocN) != 0 ? 0x80 : 0) | ((w.live & kLocV) != 0 ? 0x40 : 0) |
        ((w.live & kLocZ) != 0 ? 0x02 : 0) | ((w.live & kLocC) != 0 ? 0x01 : 0));
    wrong += Popcount((x.p ^ t.p) & flags);
    for (std::size_t i = 0; i < w.cells.size(); i++) {
        if (w.cells[i].live) {
            wrong += Popcount(static_cast<unsigned>(x.cells[i] ^ t.cells[i]));
        }
    }
    return wrong;
}

struct Chain {
    const Window* w = nullptr;
    const std::vector<SoIns>* set = nullptr;
    const std::vector<SoState>* vectors = nullptr;
    const std::vector<SoState>* targets = nullptr;
    VectorRng rng;
};

std::vector<SoIns> CodeOf(const Chain& c, const std::vector<int>& slots) {
    std::vector<SoIns> code;
    for (int slot : slots) {
        if (slot >= 0) {
            code.push_back((*c.set)[static_cast<std::size_t>(slot)]);
        }
    }
    return code;
}

void Score(const Chain& c, const std::vector<int>& slots, int& wrong, int& cycles, int& bytes) {
    std::vector<SoIns> code = CodeOf(c, slots);
    wrong = 0;
    for (std::size_t v = 0; v < c.vectors->size(); v++) {
        SoState s = (*c.vectors)[v];
        ExecAll(code, c.w->cells.data(), s);
        wrong += WrongBits(*c.w, s, (*c.targets)[v]);
    }
    cycles = 0;
    bytes = 0;
    for (const SoIns& ins : code) {
        cycles += InsCycles(ins);
        bytes += InsBytes(ins);
    }
}

// the same instruction with another operand, when the set has one
int SameMnemonic(Chain& c, int slot) {
    const std::vector<SoIns>& set = *c.set;
    Mn mn = set[static_cast<std::size_t>(slot)].mn;
    std::vector<int> options;
    for (std::size_t i = 0; i < set.size(); i++) {
        if (set[i].mn == mn) {
            options.push_back(static_cast<int>(i));
        }
    }
    return options[c.rng.Next() % options.size()];
}

void Mutate(Chain& c, std::vector<int>& slots) {
    std::size_t n = slots.size();
    std::size_t at = c.rng.Next() % n;
    int set_size = static_cast<int>(c.set->size());
    switch (c.rng.Next() % 5) {
    case 0:
        slots[at] = static_cast<int>(c.rng.Next() % static_cast<std::uint64_t>(set_size));
        break;
    case 1:
        if (slots[at] >= 0) {
            slots[at] = SameMnemonic(c, slots[at]);
        }
        break;
    case 2: {
        std::size_t other = c.rng.Next() % n;
        int t = slots[at];
        slots[at] = slots[other];
        slots[other] = t;
        break;
    }
    case 3:
        slots[at] = -1;
        break;
    default:
        if (slots[at] < 0) {
            slots[at] = static_cast<int>(c.rng.Next() % static_cast<std::uint64_t>(set_size));
        }
        break;
    }
}

int SetIndexOf(const std::vector<SoIns>& set, const SoIns& ins) {
    for (std::size_t i = 0; i < set.size(); i++) {
        const SoIns& s = set[i];
        if (s.mn == ins.mn && s.mode == ins.mode && s.imm == ins.imm && s.cell == ins.cell) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

std::vector<std::vector<SoIns>> StochasticSearch(const Window& w, const Facts& facts,
                                                 const SearchOptions& options, std::uint64_t& steps,
                                                 int& rejected) {
    std::vector<std::vector<SoIns>> accepted;
    std::vector<SoIns> set = CandidateSet(w);
    if (set.empty()) {
        return accepted;
    }
    std::vector<SoState> vectors = MakeVectors(w, facts, options.vectors, options.seed);
    std::vector<SoState> targets = vectors;
    for (SoState& t : targets) {
        ExecAll(w.code, w.cells.data(), t);
    }
    Chain c;
    c.w = &w;
    c.set = &set;
    c.vectors = &vectors;
    c.targets = &targets;
    c.rng.s = options.seed * 0xD1B54A32D192ED03ull + 5;
    // start from the window; an instruction the set lacks starts empty
    std::vector<int> cur;
    for (const SoIns& ins : w.code) {
        cur.push_back(SetIndexOf(set, ins));
    }
    int wrong = 0;
    int cycles = 0;
    int bytes = 0;
    Score(c, cur, wrong, cycles, bytes);
    double cur_cost = kWrongWeight * wrong + cycles;
    int best_cycles = options.bound_cycles >= 0 ? options.bound_cycles : w.cycles;
    int best_bytes = options.bound_bytes >= 0 ? options.bound_bytes : w.bytes;
    for (steps = 0; steps < options.stochastic_steps; steps++) {
        std::vector<int> next = cur;
        Mutate(c, next);
        Score(c, next, wrong, cycles, bytes);
        double cost = kWrongWeight * wrong + cycles;
        double r = static_cast<double>(c.rng.Next() % 1000000) / 1000000.0;
        if (cost > cur_cost && r >= std::exp(-(cost - cur_cost) / kTemperature)) {
            continue;
        }
        cur = next;
        cur_cost = cost;
        bool better = cycles < best_cycles || (cycles == best_cycles && bytes < best_bytes);
        if (wrong != 0 || !better) {
            continue;
        }
        std::vector<SoIns> code = CodeOf(c, cur);
        EquivResult e =
            CheckEquivalent(w, w.code, code, facts, options.verify_vectors, 0, options.seed + 13);
        if (!e.equal) {
            rejected++;
            continue;
        }
        best_cycles = cycles;
        best_bytes = bytes;
        accepted.push_back(code);
    }
    return accepted;
}

} // namespace tisple
