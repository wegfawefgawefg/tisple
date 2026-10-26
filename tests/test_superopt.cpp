// the window superoptimizer: its evaluator against the cycle-exact core, its equivalence
// check on known-different and known-equal pairs, and searches with known answers

#include "superopt/search.hpp"
#include "test_gaps.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

using tisple::Mn;
using tisple::Mode;
using tisple::RawIns;

RawIns R(Mn mn, Mode mode = Mode::Imp, std::uint16_t operand = 0) {
    RawIns r;
    r.mn = mn;
    r.mode = mode;
    r.operand = operand;
    return r;
}

const std::vector<RawIns> kAllLive = {R(Mn::RTS)};

bool Window(const std::vector<RawIns>& code, const std::vector<RawIns>& after,
            tisple::Window& w) {
    std::string why;
    bool ok = tisple::MakeWindow(code, after, w, why);
    if (!ok) {
        std::fprintf(stderr, "window not made: %s\n", why.c_str());
    }
    return ok;
}

// a window's own instructions as candidate code: the same cells
std::vector<tisple::SoIns> CodeOf(const tisple::Window& w, std::size_t first, std::size_t count) {
    return std::vector<tisple::SoIns>(w.code.begin() + static_cast<std::ptrdiff_t>(first),
                                      w.code.begin() + static_cast<std::ptrdiff_t>(first + count));
}

tisple::SearchOptions Quick() {
    tisple::SearchOptions o;
    o.max_nodes = 2000000;
    return o;
}

// every opcode the evaluator models, in every mode, on the simulator: the evaluator must
// agree on registers, flags and memory
void TestEvaluatorMatchesSimulator(TestResults& results) {
    const Mode modes[] = {Mode::Imp,  Mode::Acc,  Mode::Imm,  Mode::Zp,   Mode::ZpX, Mode::ZpY,
                          Mode::Abs,  Mode::AbsX, Mode::AbsY, Mode::IndX, Mode::IndY};
    int checked = 0;
    for (int m = 0; m < static_cast<int>(Mn::Count); m++) {
        Mn mn = static_cast<Mn>(m);
        if (!tisple::IsModelled(mn)) {
            continue;
        }
        for (Mode mode : modes) {
            if (tisple::OpcodeFor(mn, mode) < 0) {
                continue;
            }
            std::uint16_t operand = mode == Mode::Imm   ? 0x5A
                                    : mode == Mode::Abs ? 0x1234
                                    : (mode == Mode::AbsX || mode == Mode::AbsY) ? 0x1300
                                                                                 : 0x40;
            tisple::Window w;
            if (!Window({R(mn, mode, operand)}, kAllLive, w)) {
                CHECK(results, false);
                continue;
            }
            tisple::Facts any;
            std::string why;
            bool same = tisple::CheckOnSimulator(w, w.code, w.code, any, 300, 7, why);
            if (!same) {
                std::fprintf(stderr, "%s: %s\n", tisple::CodeText(w.code, w.cells.data()).c_str(),
                             why.c_str());
            }
            CHECK(results, same);
            checked++;
        }
    }
    CHECK(results, checked > 120);
}

void TestKnownDifferent(TestResults& results) {
    tisple::Facts any;
    struct Pair {
        std::vector<RawIns> a;
        std::vector<RawIns> b;
    };
    // each pair differs somewhere; tested with random vectors and with every input alone
    const Pair pairs[] = {
        {{R(Mn::TAX)}, {R(Mn::TAY)}},
        {{R(Mn::ASL, Mode::Acc)}, {R(Mn::ROL, Mode::Acc)}},
        {{R(Mn::LDA, Mode::Imm, 0)}, {R(Mn::LDA, Mode::Imm, 1)}},
        {{R(Mn::CLC), R(Mn::ADC, Mode::Imm, 1)}, {R(Mn::ADC, Mode::Imm, 1)}},
        {{R(Mn::LDA, Mode::Zp, 0x10), R(Mn::AND, Mode::Zp, 0x11)},
         {R(Mn::LDA, Mode::Zp, 0x10), R(Mn::ORA, Mode::Zp, 0x11)}},
        {{R(Mn::INC, Mode::Zp, 0x10)}, {R(Mn::DEC, Mode::Zp, 0x10)}},
        {{R(Mn::CMP, Mode::Imm, 0x80)}, {R(Mn::BIT, Mode::Zp, 0x10)}},
        {{R(Mn::SEC), R(Mn::SBC, Mode::Imm, 1)}, {R(Mn::CLC), R(Mn::SBC, Mode::Imm, 1)}},
    };
    for (const Pair& p : pairs) {
        // one window holding both, so the cells line up
        std::vector<RawIns> both = p.a;
        both.insert(both.end(), p.b.begin(), p.b.end());
        tisple::Window w;
        CHECK(results, Window(both, kAllLive, w));
        std::vector<tisple::SoIns> a = CodeOf(w, 0, p.a.size());
        std::vector<tisple::SoIns> b = CodeOf(w, p.a.size(), p.b.size());
        tisple::EquivResult random = tisple::CheckEquivalent(w, a, b, any, 512, 0, 3);
        tisple::EquivResult every = tisple::CheckEquivalent(w, a, b, any, 0, 24, 3);
        CHECK(results, !random.equal);
        CHECK(results, !every.equal);
        std::string why;
        CHECK(results, !tisple::CheckOnSimulator(w, a, b, any, 64, 5, why));
    }
    // a difference on one input in 65536: only trying every input finds it
    tisple::Window w;
    CHECK(results, Window({R(Mn::LDA, Mode::Zp, 0x10), R(Mn::EOR, Mode::Zp, 0x11),
                           R(Mn::CMP, Mode::Imm, 0x5A), R(Mn::LDA, Mode::Zp, 0x10),
                           R(Mn::EOR, Mode::Zp, 0x11)},
                          kAllLive, w));
    // CMP leaves C = (m0 ^ m1) >= $5A; the later LDA/EOR redo A, N, Z. A candidate without
    // the CMP differs in C on most inputs: found either way
    tisple::EquivResult r = tisple::CheckEquivalent(w, w.code, CodeOf(w, 3, 2), any, 0, 24, 3);
    CHECK(results, !r.equal);
}

void TestKnownEqual(TestResults& results) {
    tisple::Facts any;
    // CLC / ADC #1 and SEC / ADC #0 agree on A, every flag
    tisple::Window w;
    CHECK(results, Window({R(Mn::CLC), R(Mn::ADC, Mode::Imm, 1), R(Mn::SEC),
                           R(Mn::ADC, Mode::Imm, 0)},
                          kAllLive, w));
    tisple::EquivResult r = tisple::CheckEquivalent(w, CodeOf(w, 0, 2), CodeOf(w, 2, 2), any, 512,
                                                    24, 3);
    CHECK(results, r.equal && r.proof == tisple::Proof::kProved);
    std::string why;
    CHECK(results, tisple::CheckOnSimulator(w, CodeOf(w, 0, 2), CodeOf(w, 2, 2), any, 64, 5, why));
}

void TestLoadStoreLoad(TestResults& results) {
    // LDA #0 / STA x / LDA #0 is LDA #0 / STA x, proved
    tisple::Window w;
    CHECK(results, Window({R(Mn::LDA, Mode::Imm, 0), R(Mn::STA, Mode::Zp, 0x40),
                           R(Mn::LDA, Mode::Imm, 0)},
                          kAllLive, w));
    tisple::SearchResult r = tisple::Superoptimize(w, tisple::Facts(), Quick());
    CHECK(results, r.found && r.complete);
    CHECK(results, r.cycles == 5 && r.bytes == 4 && r.proof == tisple::Proof::kProved);
    CHECK(results, tisple::CodeText(r.code, w.cells.data()) == "LDA #$00 / STA $40");
    // the stochastic search finds it too, tested at least
    tisple::SearchOptions mcmc = Quick();
    mcmc.stochastic_steps = 20000;
    tisple::SearchResult s = tisple::Superoptimize(w, tisple::Facts(), mcmc);
    CHECK(results, s.found && s.cycles == 5);
}

void TestCarryFact(TestResults& results) {
    // CLC / ADC #1 has nothing cheaper; with the carry known clear on entry it is ADC #1
    tisple::Window w;
    CHECK(results, Window({R(Mn::CLC), R(Mn::ADC, Mode::Imm, 1)}, kAllLive, w));
    tisple::SearchResult exact = tisple::Superoptimize(w, tisple::Facts(), Quick());
    CHECK(results, !exact.found && exact.complete);
    tisple::Facts carry_clear;
    carry_clear.hi[5] = 0;
    tisple::SearchResult with = tisple::Superoptimize(w, carry_clear, Quick());
    CHECK(results, with.found && with.cycles == 2);
    CHECK(results, tisple::CodeText(with.code, w.cells.data()) == "ADC #$01");
    tisple::Facts needed = tisple::NeededFacts(w, with.code, carry_clear, Quick());
    CHECK(results, tisple::FactsText(w, needed) == "C=0");
}

void TestDeadAfter(TestResults& results) {
    // A, N, Z and $10 are all overwritten before they are read (X, Y, C, V are live at the
    // return but untouched): nothing is left to do
    tisple::Window w;
    CHECK(results, Window({R(Mn::LDA, Mode::Imm, 1), R(Mn::STA, Mode::Zp, 0x10)},
                          {R(Mn::LDA, Mode::Imm, 2), R(Mn::STA, Mode::Zp, 0x10), R(Mn::RTS)}, w));
    CHECK(results, w.live == (tisple::kLocX | tisple::kLocY | tisple::kLocC | tisple::kLocV) &&
                       !w.cells[0].live);
    tisple::SearchResult r = tisple::Superoptimize(w, tisple::Facts(), Quick());
    CHECK(results, r.found && r.code.empty());
    // but a branch on Z right after keeps the load
    CHECK(results, Window({R(Mn::LDA, Mode::Imm, 1), R(Mn::STA, Mode::Zp, 0x10)},
                          {R(Mn::BNE, Mode::Rel)}, w));
    CHECK(results, (w.live & tisple::kLocZ) != 0 && w.cells[0].live);
}

void TestTransferForReload(TestResults& results) {
    // TXA / STA $39 / LDA #0 / STA $1B / LDA $39 (Life's row_of): STX $39 / LDA #0 /
    // STA $1B / TXA is 3 cycles cheaper
    tisple::Window w;
    CHECK(results, Window({R(Mn::TXA), R(Mn::STA, Mode::Zp, 0x39), R(Mn::LDA, Mode::Imm, 0),
                           R(Mn::STA, Mode::Zp, 0x1B), R(Mn::LDA, Mode::Zp, 0x39)},
                          kAllLive, w));
    tisple::SearchResult r = tisple::Superoptimize(w, tisple::Facts(), Quick());
    CHECK(results, r.found && r.complete && r.cycles == w.cycles - 3);
    CHECK(results, r.proof == tisple::Proof::kProved);
}

} // namespace

void RunSuperoptTests(TestResults& results) {
    TestEvaluatorMatchesSimulator(results);
    TestKnownDifferent(results);
    TestKnownEqual(results);
    TestLoadStoreLoad(results);
    TestCarryFact(results);
    TestDeadAfter(results);
    TestTransferForReload(results);
}

} // namespace tisple_tests
