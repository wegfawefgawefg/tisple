// a division routine's operands worked out straight into rt_a and rt_b (gen_div.cpp): only
// when the other operand cannot call a routine that sets them, so operands with divisions,
// multiplies and calls of their own go through temporaries as before; an unsigned byte
// division compares inline and calls rt.divu8.ge only when the dividend is at least the
// divisor

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

void CheckPairs(TestResults& results, const std::string& text) {
    std::vector<std::uint16_t> xs = {0,    1,    2,     3,     7,      0x7F,   0x80,   0x81,
                                     0xFE, 0xFF, 0x100, 0x1FF, 0x7FFF, 0x8000, 0xC001, 0xFFFF};
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c);
    for (std::uint16_t x : xs) {
        for (std::uint16_t y : xs) {
            ok = ok && DiffCall(d, "t.f", {x, y});
        }
    }
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

void TestShapes(TestResults& results) {
    // dividend, divisor: plain, worked out, with a division or multiply or call inside
    const char* sides[] = {"x", "(+ x 3)", "(^ x (>> y 2))", "(/ x (| y 1))", "(* x y)", "(h x y)"};
    for (const char* type : {"u8", "i8", "u16", "i16"}) {
        std::string t = type;
        for (const char* a : sides) {
            for (const char* b : sides) {
                for (const char* op : {"/", "%"}) {
                    std::string text = "(let k " + t + " 0)\n(fn h ((x " + t + ") (y " + t + ")) " +
                                       t + " (set k (+ k 1)) (- x y))\n(fn f ((x " + t + ") (y " +
                                       t + ")) " + t + " (" + op + " " + a + " " + std::string(b) +
                                       "))";
                    CheckPairs(results, text);
                }
            }
        }
    }
    // a / and a % of the same worked-out operands still divide once
    CheckPairs(results, "(let r u16 0)\n(fn f ((x u16) (y u16)) u16\n"
                        "    (set r (% (+ x 1) (| y 3))) (+ r (/ (+ x 1) (| y 3))))");
    // a let read once is worked out where it is read: a division in its value sets rt_a
    // while the divisor is worked out (fuzz seed 4046043)
    for (const char* type : {"u8", "i8", "u16", "i16"}) {
        std::string t = type;
        CheckPairs(results, "(let g " + t + " 0)\n(fn f ((x " + t + ") (y u8)) " + t +
                                "\n    (set g x)\n    (let l " + t +
                                " (>> (% g (| g 1)) (& y 15)))\n    (set g (/ (- g 105) (| l 1)))"
                                "\n    g)");
    }
}

// instructions of t.f that name its temporaries
int TempUses(const Compiled& c) {
    int n = 0;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        for (std::size_t i = span.first; span.name == "t.f" && i < span.end; i++) {
            const tisple::AsmItem& item = c.code.as.items[i];
            bool temps =
                item.kind == tisple::ItemKind::Ins && item.value.sym >= 0 &&
                c.code.as.symbols[static_cast<std::size_t>(item.value.sym)].name == "t.f.temps";
            n += temps ? 1 : 0;
        }
    }
    return n;
}

void TestNoCopies(TestResults& results) {
    // the operands' bytes are stored once, into rt_a and rt_b, not into temporaries first
    Compiled w = CompileText("(let q u16 0)\n"
                             "(fn f ((x u16) (y u16)) void (set q (/ (+ x 1) (| y 1))))");
    CHECK(results, w.ok && TempUses(w) == 0);
    Compiled b = CompileText("(fn f ((x u8) (y u8)) u8 (/ (+ x 1) (| y 1)))");
    CHECK(results, CountMn(b, "t.f", tisple::Mn::STA) == 2);
}

// an unsigned word division that no other division shares compares inline and calls
// rt.divu16.ge only for a dividend at least the divisor, with both results right either way
void TestDivU16Inline(TestResults& results) {
    for (const char* op : {"/", "%"}) {
        std::string text = "(fn f ((x u16) (y u16)) u16 (" + std::string(op) + " x y))";
        Compiled c = CompileText(text);
        CHECK(results, CountMn(c, "t.f", tisple::Mn::CMP) == 2);
        CheckPairs(results, text);
        // into a variable, and with the routine's other entry kept for a / and % pair
        CheckPairs(results, "(let q u16 0)\n(fn f ((x u16) (y u16)) u16 (set q (" +
                                std::string(op) + " (+ x 7) (| y 2))) q)");
    }
}

std::uint16_t SymbolAt(const tisple::Assembly& as, const std::string& name) {
    for (const tisple::AsmSymbol& s : as.symbols) {
        if (s.name == name && s.defined) {
            return static_cast<std::uint16_t>(s.value);
        }
    }
    return 0;
}

// an unsigned byte division that no other division shares compares inline and enters the
// routine past its own compare; the entry needs no carry (it may be called with it clear)
void TestDivU8Entry(TestResults& results) {
    Compiled c = CompileText("(fn f ((x u8) (y u8)) u8 (/ x y))\n"
                             "(fn g ((x u8) (y u8)) u8 (% x y))");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::CMP) == 1);
    CHECK(results, CountMn(c, "t.g", tisple::Mn::CMP) == 1);
    // a % and a / of the same operands, either order: one compare, one call, and the way
    // past the call leaves both results where the routine does
    for (const char* first : {"%", "/"}) {
        std::string f = first;
        std::string s = f == "%" ? "/" : "%";
        std::string text =
            "(let q u8 0)\n(fn f ((x u8) (y u8)) u8 (set q (" + f + " x y)) (+ q (" + s + " x y)))";
        Compiled pair = CompileText(text);
        CHECK(results, CountMn(pair, "t.f", tisple::Mn::CMP) == 1);
        Diff d;
        bool ok = StartDiff(d, pair);
        for (std::uint16_t x = 0; x < 256 && ok; x++) {
            for (std::uint16_t y = 0; y < 256 && ok; y += 3) {
                ok = DiffCall(d, "t.f", {x, y});
            }
        }
        if (!ok) {
            std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
        }
        CHECK(results, ok);
    }
    std::uint16_t ge = SymbolAt(c.code.as, "rt.divu8.ge");
    std::uint16_t rt_a = SymbolAt(c.code.as, "rt_a");
    std::uint16_t rt_b = SymbolAt(c.code.as, "rt_b");
    std::uint16_t rt_r = SymbolAt(c.code.as, "rt_r");
    CHECK(results, ge != 0 && rt_a != 0);
    tisple::SimRunner run;
    std::string error;
    if (!tisple::StartCompiled(run, c.code, error) || ge == 0) {
        CHECK(results, false);
        return;
    }
    tisple::Machine& m = *run.machine;
    const std::uint8_t call[] = {0x20, static_cast<std::uint8_t>(ge & 0xFF),
                                 static_cast<std::uint8_t>(ge >> 8), 0x00};
    int wrong = 0;
    for (std::uint32_t b = 0; b < 256; b++) {
        for (std::uint32_t a = b; a < 256; a++) {
            tisple::LoadBytes(m, 0x0300, call, sizeof call);
            m.mem[rt_a] = static_cast<std::uint8_t>(a);
            m.mem[rt_b] = static_cast<std::uint8_t>(b);
            tisple::Regs regs;
            regs.pc = 0x0300;
            regs.a = static_cast<std::uint8_t>(a);
            regs.p = tisple::kFlagU | tisple::kFlagI; // the carry clear
            tisple::SetRegs(m, regs);
            tisple::RunOptions options;
            options.max_cycles = 10000;
            tisple::RunResult r = tisple::Run(m, options);
            std::uint32_t q = b == 0 ? 0xFF : a / b;
            std::uint32_t rem = b == 0 ? a : a % b;
            bool ok = r.reason == tisple::StopReason::kBrk && r.regs.a == q && m.mem[rt_a] == q &&
                      m.mem[rt_r] == rem;
            wrong += ok ? 0 : 1;
        }
    }
    CHECK(results, wrong == 0);
}

} // namespace

void RunBackendRtArgsTests(TestResults& results) {
    TestShapes(results);
    TestNoCopies(results);
    TestDivU16Inline(results);
    TestDivU8Entry(results);
}

} // namespace tisple_tests
