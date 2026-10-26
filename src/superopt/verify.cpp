#include "superopt/verify.hpp"

#include "emu/harness.hpp"

#include <cmath>
#include <cstdio>
#include <memory>

namespace tisple {

namespace {

constexpr std::uint8_t kFlagMask[4] = {0x80, 0x02, 0x01, 0x40}; // N Z C V
constexpr std::uint8_t kLocBit[7] = {kLocA, kLocX, kLocY, kLocN, kLocZ, kLocC, kLocV};
constexpr std::uint16_t kCodeAt = 0xF000;

std::uint8_t GetLoc(const SoState& s, int loc) {
    switch (loc) {
    case 0:
        return s.a;
    case 1:
        return s.x;
    case 2:
        return s.y;
    case 3:
    case 4:
    case 5:
    case 6:
        return (s.p & kFlagMask[loc - 3]) != 0 ? 1 : 0;
    default:
        return s.cells[loc - 7];
    }
}

void SetLoc(SoState& s, int loc, std::uint8_t v) {
    switch (loc) {
    case 0:
        s.a = v;
        break;
    case 1:
        s.x = v;
        s.x0 = v;
        break;
    case 2:
        s.y = v;
        s.y0 = v;
        break;
    case 3:
    case 4:
    case 5:
    case 6:
        s.p = static_cast<std::uint8_t>((s.p & ~kFlagMask[loc - 3]) | (v != 0 ? kFlagMask[loc - 3] : 0));
        break;
    default:
        s.cells[loc - 7] = v;
        break;
    }
}

int LocCount(const Window& w) {
    return 7 + static_cast<int>(w.cells.size());
}

// kLoc register and flag bits as location bits
std::uint32_t LocBits(std::uint8_t mask) {
    std::uint32_t bits = 0;
    for (int i = 0; i < 7; i++) {
        if ((mask & kLocBit[i]) != 0) {
            bits |= 1u << i;
        }
    }
    return bits;
}

struct Usage {
    std::uint32_t inputs = 0; // read before written
    std::uint32_t written = 0;
};

Usage UsageOf(const std::vector<SoIns>& code) {
    Usage u;
    for (const SoIns& ins : code) {
        // the value an instruction reads, not the index that finds its operand
        Mode value_mode = ins.cell >= 0 ? Mode::Zp : ins.mode;
        LocEffects e = EffectsOf(ins.mn, value_mode);
        std::uint32_t reads = LocBits(e.reads);
        std::uint32_t writes = LocBits(e.writes);
        if (ins.cell >= 0) {
            std::uint32_t bit = 1u << (7 + ins.cell);
            if (e.mem_read) {
                reads |= bit;
            }
            if (e.mem_write) {
                writes |= bit;
            }
        }
        u.inputs |= reads & ~u.written;
        u.written |= writes;
    }
    return u;
}

std::uint32_t LiveBits(const Window& w) {
    std::uint32_t bits = LocBits(w.live);
    for (std::size_t i = 0; i < w.cells.size(); i++) {
        if (w.cells[i].live) {
            bits |= 1u << (7 + i);
        }
    }
    return bits;
}

std::uint8_t PickIn(VectorRng& rng, std::uint8_t lo, std::uint8_t hi) {
    std::uint64_t r = rng.Next();
    unsigned span = static_cast<unsigned>(hi - lo) + 1;
    if (r % 4 == 0) {
        // edge values, kept inside the range
        const std::uint8_t edges[] = {lo, hi, 0x00, 0xFF, 0x7F, 0x80, 0x01, 0xFE};
        std::uint8_t v = edges[(r >> 8) % 8];
        if (v >= lo && v <= hi) {
            return v;
        }
    }
    return static_cast<std::uint8_t>(lo + (r >> 16) % span);
}

// where each cell lives on the simulator for one vector; false on a clash
struct SimLayout {
    std::uint16_t ea[kMaxCells] = {};
    std::uint16_t pointer[kMaxCells] = {}; // indirect cells: the pointer's zero-page byte
};

bool Claim(std::vector<std::uint16_t>& used, std::uint16_t address) {
    for (std::uint16_t u : used) {
        if (u == address) {
            return false;
        }
    }
    used.push_back(address);
    return true;
}

bool PlanLayout(const Window& w, const SoState& s, SimLayout& out) {
    std::vector<std::uint16_t> used;
    for (std::uint16_t a = kCodeAt; a < kCodeAt + 48; a++) {
        used.push_back(a);
    }
    for (std::size_t i = 0; i < w.cells.size(); i++) {
        const Cell& c = w.cells[i];
        std::uint16_t base = static_cast<std::uint16_t>(0x4000 + 0x200 * i);
        std::uint16_t ea = 0;
        switch (c.mode) {
        case Mode::ZpX:
            ea = static_cast<std::uint8_t>(c.operand + s.x);
            break;
        case Mode::ZpY:
            ea = static_cast<std::uint8_t>(c.operand + s.y);
            break;
        case Mode::AbsX:
            ea = static_cast<std::uint16_t>(c.operand + s.x);
            break;
        case Mode::AbsY:
            ea = static_cast<std::uint16_t>(c.operand + s.y);
            break;
        case Mode::IndX:
            out.pointer[i] = static_cast<std::uint8_t>(c.operand + s.x);
            ea = base;
            break;
        case Mode::IndY:
            out.pointer[i] = static_cast<std::uint8_t>(c.operand);
            ea = static_cast<std::uint16_t>(base + s.y);
            break;
        default:
            ea = c.operand;
            break;
        }
        out.ea[i] = ea;
        if (!Claim(used, ea)) {
            return false;
        }
        if (c.mode == Mode::IndX || c.mode == Mode::IndY) {
            if (!Claim(used, out.pointer[i]) ||
                !Claim(used, static_cast<std::uint8_t>(out.pointer[i] + 1))) {
                return false;
            }
        }
    }
    return true;
}

bool Assemble(const std::vector<SoIns>& code, const Window& w, std::vector<std::uint8_t>& out) {
    out.clear();
    for (const SoIns& ins : code) {
        int opcode = OpcodeFor(ins.mn, ins.mode);
        if (opcode < 0) {
            return false;
        }
        out.push_back(static_cast<std::uint8_t>(opcode));
        std::uint16_t v = ins.mode == Mode::Imm ? ins.imm
                          : ins.cell >= 0       ? w.cells[static_cast<std::size_t>(ins.cell)].operand
                                                : 0;
        int size = ModeSize(ins.mode);
        if (size >= 2) {
            out.push_back(static_cast<std::uint8_t>(v & 0xFF));
        }
        if (size == 3) {
            out.push_back(static_cast<std::uint8_t>(v >> 8));
        }
    }
    return true;
}

// runs code on the simulator from state s; the result in evaluator terms
bool RunOnSim(Machine& m, const Window& w, const std::vector<SoIns>& code, const SoState& s,
              const SimLayout& layout, SoState& out) {
    std::vector<std::uint8_t> bytes;
    if (!Assemble(code, w, bytes)) {
        return false;
    }
    InitMachine(m);
    for (std::size_t i = 0; i < w.cells.size(); i++) {
        const Cell& c = w.cells[i];
        m.mem[layout.ea[i]] = s.cells[i];
        if (c.mode == Mode::IndX || c.mode == Mode::IndY) {
            std::uint16_t base = static_cast<std::uint16_t>(0x4000 + 0x200 * i);
            m.mem[layout.pointer[i]] = static_cast<std::uint8_t>(base & 0xFF);
            m.mem[static_cast<std::uint8_t>(layout.pointer[i] + 1)] =
                static_cast<std::uint8_t>(base >> 8);
        }
    }
    LoadBytes(m, kCodeAt, bytes.data(), bytes.size());
    Regs regs;
    regs.pc = kCodeAt;
    regs.a = s.a;
    regs.x = s.x;
    regs.y = s.y;
    regs.p = static_cast<std::uint8_t>((s.p & 0xC3) | kFlagU | kFlagI);
    SetRegs(m, regs);
    RunOptions options;
    options.stop_pc = static_cast<std::int32_t>(kCodeAt + bytes.size());
    options.max_cycles = 1000;
    RunResult result = Run(m, options);
    if (result.reason != StopReason::kStopAddress) {
        return false;
    }
    out = s;
    out.a = result.regs.a;
    out.x = result.regs.x;
    out.y = result.regs.y;
    out.p = static_cast<std::uint8_t>(result.regs.p & 0xC3);
    for (std::size_t i = 0; i < w.cells.size(); i++) {
        out.cells[i] = m.mem[layout.ea[i]];
    }
    return true;
}

bool SameAll(const Window& w, const SoState& x, const SoState& y) {
    if (x.a != y.a || x.x != y.x || x.y != y.y || (x.p & 0xC3) != (y.p & 0xC3)) {
        return false;
    }
    for (std::size_t i = 0; i < w.cells.size(); i++) {
        if (x.cells[i] != y.cells[i]) {
            return false;
        }
    }
    return true;
}

std::string Hex2(unsigned v) {
    char text[8];
    std::snprintf(text, sizeof(text), "$%02X", v);
    return text;
}

} // namespace

Facts::Facts() {
    for (int i = 0; i < kLocCount; i++) {
        lo[i] = 0;
        hi[i] = (i >= 3 && i < 7) ? 1 : 0xFF;
    }
}

std::uint64_t VectorRng::Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

SoState RandomState(const Window& w, const Facts& facts, VectorRng& rng) {
    SoState s;
    for (int loc = 0; loc < LocCount(w); loc++) {
        SetLoc(s, loc, PickIn(rng, facts.lo[loc], facts.hi[loc]));
    }
    return s;
}

std::vector<SoState> MakeVectors(const Window& w, const Facts& facts, int count,
                                 std::uint64_t seed) {
    VectorRng rng;
    rng.s = seed * 0x2545F4914F6CDD1Dull + 0x9E3779B97F4A7C15ull;
    std::vector<SoState> out;
    for (int i = 0; i < count; i++) {
        out.push_back(RandomState(w, facts, rng));
    }
    return out;
}

bool SameLive(const Window& w, const SoState& x, const SoState& y) {
    if (x.bad != 0 || y.bad != 0) {
        return false;
    }
    if ((w.live & kLocA) != 0 && x.a != y.a) {
        return false;
    }
    if ((w.live & kLocX) != 0 && x.x != y.x) {
        return false;
    }
    if ((w.live & kLocY) != 0 && x.y != y.y) {
        return false;
    }
    std::uint8_t flags = 0;
    for (int f = 0; f < 4; f++) {
        if ((w.live & kLocBit[3 + f]) != 0) {
            flags |= kFlagMask[f];
        }
    }
    if (((x.p ^ y.p) & flags) != 0) {
        return false;
    }
    for (std::size_t i = 0; i < w.cells.size(); i++) {
        if (w.cells[i].live && x.cells[i] != y.cells[i]) {
            return false;
        }
    }
    return true;
}

const char* ProofName(Proof proof) {
    switch (proof) {
    case Proof::kProved:
        return "proved";
    case Proof::kTested:
        return "tested";
    default:
        return "none";
    }
}

EquivResult CheckEquivalent(const Window& w, const std::vector<SoIns>& a,
                            const std::vector<SoIns>& b, const Facts& facts, int random_vectors,
                            double exhaustive_bits, std::uint64_t seed) {
    EquivResult r;
    const Cell* cells = w.cells.data();
    VectorRng rng;
    rng.s = seed * 0x9E3779B97F4A7C15ull + 7;
    for (int i = 0; i < random_vectors; i++) {
        SoState x = RandomState(w, facts, rng);
        SoState y = x;
        ExecAll(a, cells, x);
        ExecAll(b, cells, y);
        r.cases++;
        if (!SameLive(w, x, y)) {
            r.counterexample = y;
            return r;
        }
    }
    r.equal = true;
    r.proof = Proof::kTested;

    // every input either reads, or a live output only one of them writes
    Usage ua = UsageOf(a);
    Usage ub = UsageOf(b);
    std::uint32_t inputs = ua.inputs | ub.inputs | (LiveBits(w) & (ua.written ^ ub.written));
    double bits = 0;
    std::vector<int> locs;
    for (int loc = 0; loc < LocCount(w); loc++) {
        if ((inputs & (1u << loc)) != 0) {
            locs.push_back(loc);
            bits += std::log2(static_cast<double>(facts.hi[loc] - facts.lo[loc]) + 1.0);
        }
    }
    r.input_bits = bits;
    if (bits > exhaustive_bits) {
        return r;
    }
    SoState base = RandomState(w, facts, rng);
    for (int loc : locs) {
        SetLoc(base, loc, facts.lo[loc]);
    }
    for (;;) {
        SoState x = base;
        SoState y = base;
        ExecAll(a, cells, x);
        ExecAll(b, cells, y);
        r.cases++;
        if (!SameLive(w, x, y)) {
            r.equal = false;
            r.proof = Proof::kNone;
            r.counterexample = base;
            return r;
        }
        // the next input, odometer style
        std::size_t k = 0;
        for (; k < locs.size(); k++) {
            int loc = locs[k];
            std::uint8_t v = GetLoc(base, loc);
            if (v < facts.hi[loc]) {
                SetLoc(base, loc, static_cast<std::uint8_t>(v + 1));
                break;
            }
            SetLoc(base, loc, facts.lo[loc]);
        }
        if (k == locs.size()) {
            break;
        }
    }
    r.proof = Proof::kProved;
    return r;
}

bool CheckOnSimulator(const Window& w, const std::vector<SoIns>& a, const std::vector<SoIns>& b,
                      const Facts& facts, int vectors, std::uint64_t seed, std::string& why) {
    std::unique_ptr<Machine> m = std::make_unique<Machine>();
    VectorRng rng;
    rng.s = seed * 0x7F4A7C159E3779B9ull + 3;
    int ran = 0;
    for (int i = 0; i < vectors * 4 && ran < vectors; i++) {
        SoState s = RandomState(w, facts, rng);
        SimLayout layout;
        if (!PlanLayout(w, s, layout)) {
            continue;
        }
        SoState sim_a;
        SoState sim_b;
        if (!RunOnSim(*m, w, a, s, layout, sim_a) || !RunOnSim(*m, w, b, s, layout, sim_b)) {
            why = "did not run on the simulator";
            return false;
        }
        SoState model_a = s;
        ExecAll(a, w.cells.data(), model_a);
        if (!SameAll(w, sim_a, model_a)) {
            why = "the evaluator disagrees with the simulator on " + CodeText(a, w.cells.data());
            return false;
        }
        if (!SameLive(w, sim_a, sim_b)) {
            why = "different on the simulator";
            return false;
        }
        ran++;
    }
    if (ran == 0) {
        why = "no vector could be laid out";
        return false;
    }
    return true;
}

bool HasFacts(const Facts& facts) {
    Facts any;
    for (int i = 0; i < kLocCount; i++) {
        if (facts.lo[i] != any.lo[i] || facts.hi[i] != any.hi[i]) {
            return true;
        }
    }
    return false;
}

std::string FactsText(const Window& w, const Facts& facts) {
    const char* names[7] = {"A", "X", "Y", "N", "Z", "C", "V"};
    Facts any;
    std::string text;
    for (int loc = 0; loc < LocCount(w); loc++) {
        if (facts.lo[loc] == any.lo[loc] && facts.hi[loc] == any.hi[loc]) {
            continue;
        }
        std::string name;
        if (loc < 7) {
            name = names[loc];
        } else {
            SoIns probe;
            probe.mn = Mn::LDA;
            probe.mode = w.cells[static_cast<std::size_t>(loc - 7)].mode;
            probe.cell = static_cast<std::int8_t>(loc - 7);
            name = InsText(probe, w.cells.data()).substr(4);
        }
        text += text.empty() ? "" : ", ";
        if (loc >= 3 && loc < 7) {
            text += name + "=" + std::to_string(facts.lo[loc]);
        } else if (facts.lo[loc] == facts.hi[loc]) {
            text += name + "=" + Hex2(facts.lo[loc]);
        } else {
            text += name + " in " + Hex2(facts.lo[loc]) + ".." + Hex2(facts.hi[loc]);
        }
    }
    return text;
}

} // namespace tisple
