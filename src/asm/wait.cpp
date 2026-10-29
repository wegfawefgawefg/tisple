#include "asm/wait.hpp"

#include <algorithm>

namespace tisple {

namespace {

// a taken branch from the instruction after it to its target costs one more cycle when the
// two are on different pages
bool Crosses(std::uint32_t after, std::uint32_t target) {
    return ((after ^ target) & 0xFF00) != 0;
}

// ---- constant waits

// a straight run of PHP/PLP pairs (7 cycles, 2 bytes), BIT $00 (3, 2), NOP (2, 1) and, after
// a loop, which leaves the carry set, BCC +0 (2, 2)
struct Pad {
    int pairs = 0;
    int bits = 0;
    int nops = 0;
    int bccs = 0;
};

// exactly `bytes` bytes that take exactly `cycles`; BCC +0 only with carry_set
bool FitPad(std::int64_t cycles, int bytes, bool carry_set, Pad& out) {
    if (cycles < 0 || bytes < 0) {
        return false;
    }
    for (int bccs = 0; 2 * bccs <= bytes && (carry_set || bccs == 0); bccs++) {
        for (int pairs = (bytes - 2 * bccs) / 2; pairs >= 0; pairs--) {
            int rest = bytes - 2 * bccs - 2 * pairs;
            // b BITs and rest - 2b NOPs take 2 rest - b cycles
            std::int64_t bits = 2 * rest - (cycles - 2 * bccs - 7 * pairs);
            if (bits >= 0 && 2 * bits <= rest) {
                out.pairs = pairs;
                out.bits = static_cast<int>(bits);
                out.nops = rest - 2 * out.bits;
                out.bccs = bccs;
                return true;
            }
        }
    }
    return false;
}

// every count of cycles exactly `bytes` bytes of pad can take, some maybe twice
std::vector<std::int64_t> PadCycles(int bytes, bool carry_set) {
    std::vector<std::int64_t> out;
    for (int bccs = 0; 2 * bccs <= bytes && (carry_set || bccs == 0); bccs++) {
        for (int pairs = 0; 2 * (bccs + pairs) <= bytes; pairs++) {
            for (int bits = 0; 2 * (bccs + pairs + bits) <= bytes; bits++) {
                int nops = bytes - 2 * (bccs + pairs + bits);
                out.push_back(2 * bccs + 7 * pairs + 3 * bits + 2 * nops);
            }
        }
    }
    return out;
}

enum class Loop : std::uint8_t {
    None,
    // PHA SEC LDA #k / L: SBC #1 BNE L / PLA: 5k + 10, a cycle more each time BNE crosses;
    // the carry is set after it
    SingleA,
    // PHA TXA LDX #k / L: DEX BNE L / TAX PLA: 5k + 12, a shorter loop to keep off a page end
    SingleX,
    // PHA TXA PHA SEC LDX #h LDA #m1 / I: SBC #1 BNE I / LDA #m DEX BNE I / PLA TAX PLA:
    // m1 counts in the first round, m in the other h - 1
    Nested,
};

constexpr int kSingleBytes = 9;
constexpr int kNestedBytes = 20;
// a single loop's pad may start with up to this many bytes before it, which moves the loop
// past a page boundary
constexpr int kMostBefore = 7;

// a pad, the loop, a pad
struct Plan {
    Loop loop = Loop::None;
    std::uint32_t count = 0;  // A's count: the single loop's, the nested one's first round
    std::uint32_t again = 0;  // A's count in the nested loop's other rounds
    std::uint32_t rounds = 0; // X's count of the nested loop's rounds
    Pad before;
    Pad after;
};

// k with (step) k = value, 1 to 255
bool Divide(std::int64_t value, std::int64_t step, std::int64_t& k) {
    if (value < step || value % step != 0 || value / step > 255) {
        return false;
    }
    k = value / step;
    return true;
}

// a single loop: L at + 4, the BNE back to it from + after; (5 + cross) k + fixed - cross
struct SingleForm {
    Loop loop = Loop::SingleA;
    std::uint32_t after = 0;
    std::int64_t fixed = 0;
    bool carry_set = false;
};
constexpr SingleForm kSingleForms[] = {{Loop::SingleA, 8, 10, true}, {Loop::SingleX, 7, 12, false}};

bool SinglePlan(std::uint32_t n, int bytes, std::uint32_t address, const SingleForm& form,
                Plan& out) {
    int pad_bytes = bytes - kSingleBytes;
    // longer than any count and pad can take
    if (n > 6 * 255 + form.fixed + 7 * (pad_bytes / 2) + 2 * (pad_bytes % 2)) {
        return false;
    }
    for (int before = 0; before <= pad_bytes && before <= kMostBefore; before++) {
        std::uint32_t at = address + static_cast<std::uint32_t>(before);
        std::int64_t cross = Crosses(at + form.after, at + 4) ? 1 : 0;
        int rest = pad_bytes - before;
        for (std::int64_t first : PadCycles(before, false)) {
            for (std::int64_t last : PadCycles(rest, form.carry_set)) {
                std::int64_t k = 0;
                if (!Divide(n - first - last - form.fixed + cross, 5 + cross, k)) {
                    continue;
                }
                FitPad(first, before, false, out.before);
                FitPad(last, rest, form.carry_set, out.after);
                out.loop = form.loop;
                out.count = static_cast<std::uint32_t>(k);
                return true;
            }
        }
    }
    return false;
}

bool NestedPlan(std::uint32_t n, int bytes, std::uint32_t address, Plan& out) {
    // I: SBC #1 at + 8; BNE I from + 12 and from + 17
    std::int64_t inner = Crosses(address + 12, address + 8) ? 1 : 0;
    std::int64_t outer = Crosses(address + 17, address + 8) ? 1 : 0;
    std::int64_t step = 5 + inner;
    int pad_bytes = bytes - kNestedBytes;
    for (std::int64_t last : PadCycles(pad_bytes, true)) {
        std::int64_t t = n - last;
        for (std::int64_t h = 2; h <= 255; h++) {
            // t = 30 + (step m1 - 1 - inner) + (h - 1) (step m - 1 - inner + 7 + outer)
            std::int64_t base = 30 - 1 - inner + (h - 1) * (6 - inner + outer);
            std::int64_t per = (h - 1) * step;
            if (base + per + step > t) {
                break;
            }
            // the largest m that leaves m1 at least 1; every m gives the same m1 mod step
            std::int64_t m = (t - base - step) / per;
            m = m > 255 ? 255 : m;
            std::int64_t m1 = 0;
            if (Divide(t - base - per * m, step, m1)) {
                FitPad(last, pad_bytes, true, out.after);
                out.loop = Loop::Nested;
                out.count = static_cast<std::uint32_t>(m1);
                out.again = static_cast<std::uint32_t>(m);
                out.rounds = static_cast<std::uint32_t>(h);
                return true;
            }
        }
    }
    return false;
}

// exactly `bytes` bytes taking exactly n cycles at this address
bool PlanAt(std::uint32_t n, int bytes, std::uint32_t address, Plan& out) {
    out = Plan();
    if (FitPad(n, bytes, false, out.after)) {
        return true;
    }
    for (const SingleForm& form : kSingleForms) {
        if (bytes >= kSingleBytes && SinglePlan(n, bytes, address, form, out)) {
            return true;
        }
    }
    return bytes >= kNestedBytes && NestedPlan(n, bytes, address, out);
}

// which loop branches cross a page at this address, for each place of the loop; equal keys
// plan alike
std::uint32_t CrossingKey(std::uint32_t address) {
    std::uint32_t key = 0;
    for (const SingleForm& form : kSingleForms) {
        for (std::uint32_t before = 0; before <= kMostBefore; before++) {
            std::uint32_t at = address + before;
            key = key << 1 | (Crosses(at + form.after, at + 4) ? 1 : 0);
        }
    }
    key = key << 1 | (Crosses(address + 12, address + 8) ? 1 : 0);
    key = key << 1 | (Crosses(address + 17, address + 8) ? 1 : 0);
    return key;
}

void EmitPad(const Pad& pad, std::vector<std::uint8_t>& out) {
    for (int i = 0; i < pad.pairs; i++) {
        out.push_back(0x08); // PHP
        out.push_back(0x28); // PLP
    }
    for (int i = 0; i < pad.bits; i++) {
        out.push_back(0x24); // BIT $00
        out.push_back(0x00);
    }
    for (int i = 0; i < pad.bccs; i++) {
        out.push_back(0x90); // BCC +0, never taken
        out.push_back(0x00);
    }
    for (int i = 0; i < pad.nops; i++) {
        out.push_back(0xEA); // NOP
    }
}

void EmitLoop(const Plan& plan, std::vector<std::uint8_t>& out) {
    std::uint8_t count = static_cast<std::uint8_t>(plan.count);
    std::uint8_t again = static_cast<std::uint8_t>(plan.again);
    std::uint8_t rounds = static_cast<std::uint8_t>(plan.rounds);
    if (plan.loop == Loop::SingleA) {
        out.insert(out.end(), {0x48, 0x38, 0xA9, count, 0xE9, 0x01, 0xD0, 0xFC, 0x68});
    } else if (plan.loop == Loop::SingleX) {
        out.insert(out.end(), {0x48, 0x8A, 0xA2, count, 0xCA, 0xD0, 0xFD, 0xAA, 0x68});
    } else if (plan.loop == Loop::Nested) {
        out.insert(out.end(), {0x48, 0x8A, 0x48, 0x38,  0xA2, rounds, 0xA9, count, 0xE9, 0x01,
                               0xD0, 0xFC, 0xA9, again, 0xCA, 0xD0,   0xF7, 0x68,  0xAA, 0x68});
    }
}

// ---- variable waits

// before the filler goes in; branches are data-dependent, so none may cross a page
//  0 PHA / SEC / LDA low / SBC #MIN / STA s / LDA high / SBC #0      (m = n - MIN)
// 14 BCS ok / LDA #0 / STA s / BCC join / ok: PHP PLP             (m = 0 below MIN)
// 24 join: STA s+1 / BEQ f0 / NOP
// 29 O: LDA #49 / SEC / I: SBC #1 / BNE I / DEC s+1 / BNE O        (256 cycles a round)
// 40 f0: LDA s / LSR A / BCS +0 / LSR A / BCC a1 / BIT $00
// 50 a1: LSR A / BCC a2 / BIT $00 / NOP
// 56 a2: SEC / L: BIT $00 / SBC #1 / BCS L / PLA                   ((m & 255) / 8 + 1 rounds)
constexpr int kVarBodyBytes = 64;
// where each instruction starts
constexpr int kVarStarts[] = {0,  1,  2,  5,  7,  9,  12, 14, 16, 18, 20, 22, 23,
                              24, 26, 28, 29, 31, 32, 34, 36, 38, 40, 42, 43, 45,
                              46, 48, 50, 51, 53, 55, 56, 57, 59, 61, 63, 64};
// a branch's offset in the body and its target's
constexpr int kVarBranches[][2] = {{14, 22}, {20, 24}, {26, 40}, {34, 32}, {38, 29},
                                   {43, 45}, {46, 50}, {51, 56}, {61, 57}};
// the shortest filler that a JMP over it at some cut makes work at every address (tests check
// every address)
constexpr int kVarFiller = 9;

void VarBody(std::uint16_t low, std::uint8_t s, std::vector<std::uint8_t>& out) {
    std::uint8_t lo = static_cast<std::uint8_t>(low & 0xFF);
    std::uint8_t hi = static_cast<std::uint8_t>(low >> 8);
    std::uint16_t high = static_cast<std::uint16_t>(low + 1);
    std::uint8_t h_lo = static_cast<std::uint8_t>(high & 0xFF);
    std::uint8_t h_hi = static_cast<std::uint8_t>(high >> 8);
    std::uint8_t s1 = static_cast<std::uint8_t>(s + 1);
    std::uint8_t min = static_cast<std::uint8_t>(kVarWaitMin);
    out = {0x48, 0x38, 0xAD, lo,   hi,   0xE9, min,  0x85, s,    0xAD, h_lo, h_hi, 0xE9,
           0x00, 0xB0, 0x06, 0xA9, 0x00, 0x85, s,    0x90, 0x02, 0x08, 0x28, 0x85, s1,
           0xF0, 0x0C, 0xEA, 0xA9, 0x31, 0x38, 0xE9, 0x01, 0xD0, 0xFC, 0xC6, s1,   0xD0,
           0xF5, 0xA5, s,    0x4A, 0xB0, 0x00, 0x4A, 0x90, 0x02, 0x24, 0x00, 0x4A, 0x90,
           0x03, 0x24, 0x00, 0xEA, 0x38, 0x24, 0x00, 0xE9, 0x01, 0xB0, 0xFA, 0x68};
}

// a JMP over the filler can go in at cut without changing any branch's offset: no branch
// jumps across it, though one may land on it
bool CutKeepsBranches(int cut) {
    for (const auto& branch : kVarBranches) {
        int low = branch[0] < branch[1] ? branch[0] : branch[1];
        int high = branch[0] + 2 > branch[1] ? branch[0] + 2 : branch[1];
        if (cut > low && cut < high) {
            return false;
        }
    }
    return true;
}

// no branch crosses a page with the JMP and filler at cut
bool CutWorks(std::uint32_t address, int cut, int filler) {
    for (const auto& branch : kVarBranches) {
        int at = branch[0] + (branch[0] >= cut ? 3 + filler : 0);
        std::uint32_t after = address + static_cast<std::uint32_t>(at) + 2;
        std::uint32_t target = after + static_cast<std::uint32_t>(branch[1] - branch[0] - 2);
        if (Crosses(after, target)) {
            return false;
        }
    }
    return true;
}

} // namespace

int WaitSize(std::uint32_t n) {
    // one address for each way the loops' branches can cross
    std::vector<std::uint32_t> kinds;
    std::vector<std::uint32_t> keys;
    for (std::uint32_t low = 0; low < 256; low++) {
        std::uint32_t key = CrossingKey(low);
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(key);
            kinds.push_back(low);
        }
    }
    for (int bytes = 1; bytes <= 64; bytes++) {
        bool all = true;
        for (std::size_t i = 0; i < kinds.size() && all; i++) {
            Plan plan;
            all = PlanAt(n, bytes, kinds[i], plan);
        }
        if (all) {
            return bytes;
        }
    }
    return 0; // n out of range
}

void WaitBytes(std::uint32_t n, int size, std::uint16_t address, std::vector<std::uint8_t>& out) {
    out.clear();
    Plan plan;
    if (!PlanAt(n, size, address, plan)) {
        return;
    }
    EmitPad(plan.before, out);
    EmitLoop(plan, out);
    EmitPad(plan.after, out);
}

int VarWaitSize() {
    return kVarBodyBytes + 3 + kVarFiller;
}

void VarWaitBytes(std::uint16_t address, std::uint16_t low, std::uint8_t scratch,
                  std::vector<std::uint8_t>& out) {
    std::vector<std::uint8_t> body;
    VarBody(low, scratch, body);
    // the last cut that works, so the JMP is at the end when nothing crosses
    int cut = kVarBodyBytes;
    for (int i = static_cast<int>(std::size(kVarStarts)); i-- > 0;) {
        int c = kVarStarts[i];
        if (CutKeepsBranches(c) && CutWorks(address, c, kVarFiller)) {
            cut = c;
            break;
        }
    }
    std::uint16_t on = static_cast<std::uint16_t>(address + cut + 3 + kVarFiller);
    out.assign(body.begin(), body.begin() + cut);
    out.push_back(0x4C); // JMP on
    out.push_back(static_cast<std::uint8_t>(on & 0xFF));
    out.push_back(static_cast<std::uint8_t>(on >> 8));
    out.insert(out.end(), static_cast<std::size_t>(kVarFiller), 0x00);
    out.insert(out.end(), body.begin() + cut, body.end());
}

} // namespace tisple
