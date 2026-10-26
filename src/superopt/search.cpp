#include "superopt/search.hpp"

#include "superopt/stochastic.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>

namespace tisple {

namespace {

struct TableEntry {
    std::uint64_t key = 0;
    std::uint8_t cycles = 0xFF;
    std::uint8_t depth = 0xFF;
};

struct Searcher {
    const Window* w = nullptr;
    const Facts* facts = nullptr;
    const SearchOptions* o = nullptr;
    std::vector<SoIns> set; // candidate instructions, cheapest first
    std::vector<int> set_cycles;
    std::vector<int> set_bytes;
    std::vector<std::uint32_t> set_reads; // registers, flags (kLoc bits) and cells (bit 7 + i)
    std::vector<std::uint32_t> set_writes;
    std::vector<SoState> targets;
    std::vector<std::vector<SoState>> stack; // states on every vector, by depth
    std::uint64_t mask[2] = {};
    std::vector<TableEntry> table;
    std::vector<SoIns> path;
    std::vector<std::size_t> path_index; // into set
    std::uint32_t needed = 0;            // cells every candidate must read
    int read_cycles[kMaxCells] = {};
    int read_bytes[kMaxCells] = {};
    int best_cycles = 0;
    int best_bytes = 0;
    std::vector<std::vector<SoIns>> accepted; // each better than the one before
    std::uint64_t nodes = 0;
    bool out_of_budget = false;
    int rejected = 0;
};

const Mn kSearchMns[] = {
    Mn::LDA, Mn::LDX, Mn::LDY, Mn::STA, Mn::STX, Mn::STY, Mn::ADC, Mn::SBC,
    Mn::AND, Mn::ORA, Mn::EOR, Mn::CMP, Mn::CPX, Mn::CPY, Mn::BIT, Mn::ASL,
    Mn::LSR, Mn::ROL, Mn::ROR, Mn::INC, Mn::DEC, Mn::INX, Mn::INY, Mn::DEX,
    Mn::DEY, Mn::TAX, Mn::TAY, Mn::TXA, Mn::TYA, Mn::CLC, Mn::SEC, Mn::CLV,
};

void AddIfValid(const Window& w, std::vector<SoIns>& set, const SoIns& ins) {
    if (OpcodeFor(ins.mn, ins.mode) < 0) {
        return;
    }
    LocEffects e = EffectsOf(ins.mn, ins.mode);
    if ((e.writes & w.fixed) != 0) {
        return;
    }
    if (ins.cell >= 0 && e.mem_write && !w.cells[static_cast<std::size_t>(ins.cell)].writable) {
        return;
    }
    set.push_back(ins);
}

void BuildSet(Searcher& s) {
    s.set = CandidateSet(*s.w);
    for (const SoIns& ins : s.set) {
        s.set_cycles.push_back(InsCycles(ins));
        s.set_bytes.push_back(InsBytes(ins));
        LocEffects e = EffectsOf(ins.mn, ins.mode);
        std::uint32_t reads = e.reads;
        std::uint32_t writes = e.writes;
        if (ins.cell >= 0) {
            std::uint32_t bit = 1u << (7 + ins.cell);
            reads |= e.mem_read ? bit : 0;
            writes |= e.mem_write ? bit : 0;
        }
        s.set_reads.push_back(reads);
        s.set_writes.push_back(writes);
    }
}

// i right after p is never worth trying: i overwrites all p wrote without reading it (p is
// useless), or the two are independent and in the other order (tried that way already)
bool SkipAfter(const Searcher& s, std::size_t p, std::size_t i) {
    std::uint32_t rp = s.set_reads[p];
    std::uint32_t wp = s.set_writes[p];
    std::uint32_t ri = s.set_reads[i];
    std::uint32_t wi = s.set_writes[i];
    std::uint32_t kills = wi & ~ri;
    if ((wp & ~kills) == 0 && (ri & wp) == 0) {
        return true;
    }
    bool independent = (rp & wi) == 0 && (wp & ri) == 0 && (wp & wi) == 0;
    return independent && i < p;
}

void BuildMask(Searcher& s) {
    SoState m;
    std::memset(&m, 0, sizeof(m));
    const Window& w = *s.w;
    m.a = (w.live & kLocA) != 0 ? 0xFF : 0;
    m.x = (w.live & kLocX) != 0 ? 0xFF : 0;
    m.y = (w.live & kLocY) != 0 ? 0xFF : 0;
    m.p = static_cast<std::uint8_t>(
        ((w.live & kLocN) != 0 ? 0x80 : 0) | ((w.live & kLocV) != 0 ? 0x40 : 0) |
        ((w.live & kLocZ) != 0 ? 0x02 : 0) | ((w.live & kLocC) != 0 ? 0x01 : 0));
    for (std::size_t i = 0; i < w.cells.size(); i++) {
        m.cells[i] = w.cells[i].live ? 0xFF : 0;
    }
    m.bad = 0xFF;
    std::memcpy(s.mask, &m, sizeof(m));
}

void Words(const SoState& st, std::uint64_t out[2]) {
    std::memcpy(out, &st, sizeof(st));
}

std::uint64_t Mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 31);
}

std::uint64_t HashStates(const std::vector<SoState>& states) {
    std::uint64_t h = 0x1234567;
    for (const SoState& st : states) {
        std::uint64_t words[2];
        Words(st, words);
        h = Mix(Mix(h, words[0]), words[1]);
    }
    return h;
}

// what is still wrong on some vector: cells (each needs its own write) and registers (each
// needs its own instruction, since none writes two; flags alone count as one)
void Wrong(const Searcher& s, const std::vector<SoState>& states, std::uint32_t& cells, int& regs) {
    std::uint64_t d0 = 0;
    std::uint64_t d1 = 0;
    for (std::size_t v = 0; v < states.size(); v++) {
        std::uint64_t a[2];
        std::uint64_t b[2];
        Words(states[v], a);
        Words(s.targets[v], b);
        d0 |= (a[0] ^ b[0]) & s.mask[0];
        d1 |= (a[1] ^ b[1]) & s.mask[1];
    }
    regs = 0;
    for (int byte = 0; byte < 3; byte++) {
        regs += ((d0 >> (8 * byte)) & 0xFF) != 0 ? 1 : 0;
    }
    if (regs == 0 && ((d0 >> 24) & 0xFF) != 0) {
        regs = 1;
    }
    // cells are bytes 4..11
    cells = 0;
    for (int c = 0; c < kMaxCells; c++) {
        std::uint64_t word = c < 4 ? d0 : d1;
        int byte = c < 4 ? 4 + c : c - 4;
        if (((word >> (8 * byte)) & 0xFF) != 0) {
            cells |= 1u << c;
        }
    }
}

// cells the live results depend on: changing one (inside the facts) changes them, so every
// candidate has to read it
std::uint32_t NeededCells(const Searcher& s, const std::vector<SoState>& vectors) {
    const Window& w = *s.w;
    std::uint32_t needed = 0;
    VectorRng rng;
    rng.s = 99;
    for (std::size_t c = 0; c < w.cells.size(); c++) {
        std::uint8_t lo = s.facts->lo[7 + c];
        std::uint8_t hi = s.facts->hi[7 + c];
        for (std::size_t v = 0; v < vectors.size() && (needed & (1u << c)) == 0; v++) {
            std::uint8_t span = static_cast<std::uint8_t>(hi - lo);
            std::uint8_t alts[4] = {lo, hi, static_cast<std::uint8_t>(lo + span / 2),
                                    static_cast<std::uint8_t>(lo + rng.Next() % (span + 1u))};
            for (std::uint8_t alt : alts) {
                SoState x = vectors[v];
                if (x.cells[c] == alt) {
                    continue;
                }
                x.cells[c] = alt;
                ExecAll(w.code, w.cells.data(), x);
                if (!w.cells[c].writable) {
                    x.cells[c] = s.targets[v].cells[c]; // passed through, not a result
                }
                if (!SameLive(w, x, s.targets[v])) {
                    needed |= 1u << c;
                    break;
                }
            }
        }
    }
    return needed;
}

// the cheapest instruction in the set that reads each cell (99: none does)
void ReadCosts(Searcher& s) {
    for (int c = 0; c < kMaxCells; c++) {
        s.read_cycles[c] = 99;
        s.read_bytes[c] = 99;
    }
    for (std::size_t i = 0; i < s.set.size(); i++) {
        for (int c = 0; c < kMaxCells; c++) {
            if ((s.set_reads[i] & (1u << (7 + c))) == 0) {
                continue;
            }
            s.read_cycles[c] = std::min(s.read_cycles[c], s.set_cycles[i]);
            s.read_bytes[c] = std::min(s.read_bytes[c], s.set_bytes[i]);
        }
    }
}

bool Better(const Searcher& s, int cycles, int bytes) {
    return cycles < s.best_cycles || (cycles == s.best_cycles && bytes < s.best_bytes);
}

void Consider(Searcher& s, int cycles, int bytes) {
    if (!Better(s, cycles, bytes)) {
        return;
    }
    EquivResult r =
        CheckEquivalent(*s.w, s.w->code, s.path, *s.facts, s.o->verify_vectors, 0, s.o->seed + 11);
    if (!r.equal) {
        s.rejected++;
        return;
    }
    s.best_cycles = cycles;
    s.best_bytes = bytes;
    s.accepted.push_back(s.path);
}

// a lower bound on what is left: each needed cell not read yet costs its cheapest read
// (which may fix one register too, or the cell itself by read-modify-write), each other
// wrong cell a store, each remaining wrong register an instruction
void LowerBound(const Searcher& s, std::uint32_t wrong_cells, int wrong_regs,
                std::uint32_t read_mask, int& cycles, int& bytes) {
    std::uint32_t unread = s.needed & ~read_mask;
    cycles = 0;
    bytes = 0;
    int reads = 0;
    for (int c = 0; c < kMaxCells; c++) {
        if ((unread & (1u << c)) != 0) {
            cycles += s.read_cycles[c];
            bytes += s.read_bytes[c];
            reads++;
        }
    }
    int stores = std::popcount(wrong_cells & ~unread);
    int regs = wrong_regs > reads ? wrong_regs - reads : 0;
    cycles += 3 * stores + 2 * regs;
    bytes += 2 * stores + regs;
}

void Search(Searcher& s, std::size_t depth, int cycles, int bytes, std::uint32_t read_mask) {
    s.nodes++;
    if (s.nodes > s.o->max_nodes) {
        s.out_of_budget = true;
        return;
    }
    const std::vector<SoState>& cur = s.stack[depth];
    std::uint32_t wrong_cells = 0;
    int wrong_regs = 0;
    Wrong(s, cur, wrong_cells, wrong_regs);
    if (wrong_cells == 0 && wrong_regs == 0 && (s.needed & ~read_mask) == 0) {
        Consider(s, cycles, bytes);
        return; // anything longer costs more
    }
    if (depth == static_cast<std::size_t>(s.o->max_len)) {
        return;
    }
    int lb_cycles = 0;
    int lb_bytes = 0;
    LowerBound(s, wrong_cells, wrong_regs, read_mask, lb_cycles, lb_bytes);
    if (!Better(s, cycles + lb_cycles, bytes + lb_bytes)) {
        return;
    }
    std::vector<SoState>& next = s.stack[depth + 1];
    const Cell* cells = s.w->cells.data();
    std::uint64_t table_mask = s.table.size() - 1;
    for (std::size_t i = 0; i < s.set.size(); i++) {
        int c2 = cycles + s.set_cycles[i];
        if (c2 > s.best_cycles) {
            break;
        }
        int b2 = bytes + s.set_bytes[i];
        if (!Better(s, c2, b2)) {
            continue;
        }
        if (!s.path_index.empty() && SkipAfter(s, s.path_index.back(), i)) {
            continue;
        }
        bool bad = false;
        for (std::size_t v = 0; v < cur.size(); v++) {
            next[v] = cur[v];
            Exec(s.set[i], cells, next[v]);
            if (next[v].bad != 0) {
                bad = true;
                break;
            }
        }
        if (bad) {
            continue;
        }
        std::uint64_t h = HashStates(next);
        TableEntry& e = s.table[h & table_mask];
        std::uint8_t d2 = static_cast<std::uint8_t>(depth + 1);
        if (e.key == h && (e.cycles < c2 || (e.cycles == c2 && e.depth <= d2))) {
            continue;
        }
        e.key = h;
        e.cycles = static_cast<std::uint8_t>(c2);
        e.depth = d2;
        s.path.push_back(s.set[i]);
        s.path_index.push_back(i);
        Search(s, depth + 1, c2, b2, read_mask | (s.set_reads[i] >> 7));
        s.path.pop_back();
        s.path_index.pop_back();
        if (s.out_of_budget) {
            return;
        }
    }
}

// the window with whatever instructions can simply go: a first bound, so the search
// prunes from the start
void SeedByDeletion(Searcher& s) {
    std::vector<SoIns> code = s.w->code;
    for (std::size_t i = code.size(); i-- > 0;) {
        std::vector<SoIns> trial = code;
        trial.erase(trial.begin() + static_cast<std::ptrdiff_t>(i));
        EquivResult r = CheckEquivalent(*s.w, s.w->code, trial, *s.facts, s.o->verify_vectors, 0,
                                        s.o->seed + 5);
        if (r.equal) {
            code = trial;
        }
    }
    int cycles = 0;
    int bytes = 0;
    for (const SoIns& ins : code) {
        cycles += InsCycles(ins);
        bytes += InsBytes(ins);
    }
    if (code.size() < s.w->code.size() && Better(s, cycles, bytes)) {
        s.best_cycles = cycles;
        s.best_bytes = bytes;
        s.accepted.push_back(code);
    }
}

// the full checks on one candidate; fills proof
bool Confirm(const Window& w, const std::vector<SoIns>& code, const Facts& facts,
             const SearchOptions& o, SearchResult& out) {
    EquivResult r =
        CheckEquivalent(w, w.code, code, facts, o.verify_vectors, o.exhaustive_bits, o.seed + 23);
    if (!r.equal) {
        out.why_rejected = "differs on some input";
        return false;
    }
    std::string why;
    if (!CheckOnSimulator(w, w.code, code, facts, o.sim_vectors, o.seed + 31, why)) {
        out.why_rejected = why;
        return false;
    }
    out.proof = r.proof;
    out.input_bits = r.input_bits;
    return true;
}

int CodeCycles(const std::vector<SoIns>& code) {
    int total = 0;
    for (const SoIns& ins : code) {
        total += InsCycles(ins);
    }
    return total;
}

int CodeBytes(const std::vector<SoIns>& code) {
    int total = 0;
    for (const SoIns& ins : code) {
        total += InsBytes(ins);
    }
    return total;
}

} // namespace

std::vector<SoIns> CandidateSet(const Window& w) {
    std::vector<SoIns> set;
    for (Mn mn : kSearchMns) {
        SoIns ins;
        ins.mn = mn;
        ins.mode = Mode::Imp;
        AddIfValid(w, set, ins);
        ins.mode = Mode::Acc;
        AddIfValid(w, set, ins);
        ins.mode = Mode::Imm;
        for (std::uint8_t imm : w.imms) {
            ins.imm = imm;
            AddIfValid(w, set, ins);
        }
        ins.imm = 0;
        for (std::size_t c = 0; c < w.cells.size(); c++) {
            ins.mode = w.cells[c].mode;
            ins.cell = static_cast<std::int8_t>(c);
            AddIfValid(w, set, ins);
        }
    }
    std::stable_sort(set.begin(), set.end(), [](const SoIns& x, const SoIns& y) {
        int cx = InsCycles(x);
        int cy = InsCycles(y);
        return cx != cy ? cx < cy : InsBytes(x) < InsBytes(y);
    });
    return set;
}

SearchResult Superoptimize(const Window& w, const Facts& facts, const SearchOptions& options) {
    auto start = std::chrono::steady_clock::now();
    SearchResult out;
    std::vector<std::vector<SoIns>> accepted;
    if (options.stochastic_steps > 0) {
        accepted = StochasticSearch(w, facts, options, out.nodes, out.rejected);
        out.complete = false;
    } else {
        Searcher s;
        s.w = &w;
        s.facts = &facts;
        s.o = &options;
        BuildSet(s);
        BuildMask(s);
        std::vector<SoState> vectors = MakeVectors(w, facts, options.vectors, options.seed);
        s.targets = vectors;
        for (SoState& t : s.targets) {
            ExecAll(w.code, w.cells.data(), t);
        }
        s.stack.assign(static_cast<std::size_t>(options.max_len) + 1, vectors);
        s.table.assign(std::size_t{1} << options.table_bits, TableEntry());
        s.best_cycles = options.bound_cycles >= 0 ? options.bound_cycles : w.cycles;
        s.best_bytes = options.bound_bytes >= 0 ? options.bound_bytes : w.bytes;
        s.needed = NeededCells(s, vectors);
        ReadCosts(s);
        SeedByDeletion(s);
        Search(s, 0, 0, 0, 0);
        accepted = s.accepted;
        out.nodes = s.nodes;
        out.complete = !s.out_of_budget;
        out.rejected = s.rejected;
    }
    // the best that passes every check
    for (std::size_t i = accepted.size(); i-- > 0;) {
        if (Confirm(w, accepted[i], facts, options, out)) {
            out.found = true;
            out.code = accepted[i];
            out.cycles = CodeCycles(out.code);
            out.bytes = CodeBytes(out.code);
            break;
        }
        out.rejected++;
    }
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return out;
}

Facts NeededFacts(const Window& w, const std::vector<SoIns>& candidate, const Facts& facts,
                  const SearchOptions& options) {
    Facts needed = facts;
    Facts any;
    for (int loc = 0; loc < kLocCount; loc++) {
        if (needed.lo[loc] == any.lo[loc] && needed.hi[loc] == any.hi[loc]) {
            continue;
        }
        Facts relaxed = needed;
        relaxed.lo[loc] = any.lo[loc];
        relaxed.hi[loc] = any.hi[loc];
        EquivResult r = CheckEquivalent(w, w.code, candidate, relaxed, options.verify_vectors,
                                        options.exhaustive_bits, options.seed + 41);
        if (r.equal) {
            needed = relaxed;
        }
    }
    return needed;
}

} // namespace tisple
