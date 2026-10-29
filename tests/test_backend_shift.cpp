// 8-bit shifts by constants the short way round, shifts by a count that may be 8 or more
// (gen_shift8.cpp, ByteFirst in gen_shift.cpp), against the interpreter

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

// f called with every (x, n) pair the lists give
void CheckPairs(TestResults& results, const std::string& text, const std::vector<std::uint16_t>& xs,
                const std::vector<std::uint16_t>& ns) {
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c);
    for (std::size_t i = 0; i < xs.size() && ok; i++) {
        for (std::size_t j = 0; j < ns.size() && ok; j++) {
            ok = DiffCall(d, "t.f", {xs[i], ns[j]});
            if (!ok) {
                std::fprintf(stderr, "x = %u, n = %u\n", xs[i], ns[j]);
            }
        }
    }
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

std::vector<std::uint16_t> EveryByte() {
    std::vector<std::uint16_t> out;
    for (std::uint16_t x = 0; x < 256; x++) {
        out.push_back(x);
    }
    return out;
}

void TestConstShifts8(TestResults& results) {
    std::vector<std::uint16_t> none = {0};
    for (int c = 0; c <= 9; c++) {
        std::string n = std::to_string(c);
        for (const char* type : {"u8", "i8"}) {
            std::string t = type;
            CheckPairs(results,
                       "(fn f ((x " + t + ") (k u8)) " + t + " (^ (>> x " + n + ") (as " + t +
                           " k)))",
                       EveryByte(), none);
            CheckPairs(results,
                       "(fn f ((x " + t + ") (k u8)) " + t + " (^ (<< x " + n + ") (as " + t +
                           " k)))",
                       EveryByte(), none);
        }
    }
    // the short forms: no seven-step runs left
    Compiled c = CompileText("(fn f ((x u8)) u8 (>> x 7))\n(fn g ((x i8)) i8 (>> x 7))\n"
                             "(fn h ((x u8)) u8 (<< x 6))\n(fn k ((x i8)) i8 (>> x 5))");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::LSR) == 0);
    CHECK(results, CountMn(c, "t.g", tisple::Mn::ROR) == 0);
    CHECK(results, CountMn(c, "t.h", tisple::Mn::ROR) == 3);
    CHECK(results, CountMn(c, "t.k", tisple::Mn::LSR) == 5);
}

void TestCountShifts(TestResults& results) {
    std::vector<std::uint16_t> xs = {0, 1, 2, 0x7F, 0x80, 0x81, 0xAA, 0xFE, 0xFF};
    std::vector<std::uint16_t> ns;
    for (std::uint16_t n = 0; n <= 20; n++) {
        ns.push_back(n);
    }
    ns.push_back(127);
    ns.push_back(128);
    ns.push_back(255);
    for (const char* type : {"u8", "i8"}) {
        std::string t = type;
        CheckPairs(results, "(fn f ((x " + t + ") (n u8)) " + t + " (>> x n))", xs, ns);
        CheckPairs(results, "(fn f ((x " + t + ") (n u8)) " + t + " (<< x n))", xs, ns);
        // the count's range below 8: no compare
        CheckPairs(results, "(fn f ((x " + t + ") (n u8)) " + t + " (>> x (& n 7)))", xs, ns);
    }
    Compiled small = CompileText("(fn f ((x u8) (n u8)) u8 (>> x (& n 7)))");
    CHECK(results, CountMn(small, "t.f", tisple::Mn::CPX) == 0);
    Compiled big = CompileText("(fn f ((x u8) (n u8)) u8 (>> x (& n 15)))");
    CHECK(results, CountMn(big, "t.f", tisple::Mn::CPX) == 1);
    // 16 bits: a byte moved first for 8 or more
    std::vector<std::uint16_t> words = {0,      1,      0x00FF, 0x0100, 0x7FFF,
                                        0x8000, 0x8001, 0xA5C3, 0xFFFF};
    for (const char* type : {"u16", "i16"}) {
        std::string t = type;
        CheckPairs(results, "(fn f ((x " + t + ") (n u8)) " + t + " (>> x n))", words, ns);
        CheckPairs(results, "(fn f ((x " + t + ") (n u8)) " + t + " (<< x n))", words, ns);
        CheckPairs(results,
                   "(let g " + t + " 0)\n(fn f ((x " + t + ") (n u8)) " + t +
                       " (set g x) (set g (>> g (& n 15))) g)",
                   words, ns);
        CheckPairs(results,
                   "(let g " + t + " 0)\n(fn f ((x " + t + ") (n u8)) " + t +
                       " (set g x) (set g (<< g (& n 15))) g)",
                   words, ns);
    }
    Compiled wide = CompileText("(fn f ((x u16) (n u8)) u16 (<< x (& n 15)))");
    CHECK(results, CountMn(wide, "t.f", tisple::Mn::PHA) == 1);
    // in a loop whose counter is in X: the count goes in Y
    CheckPairs(results,
               "(let acc u16 0)\n"
               "(fn f ((x u16) (n u8)) u16\n"
               "    (set acc 0)\n"
               "    (for i u8 0 (& n 15) (set acc (+ acc (>> x (& (+ i n) 15)))))\n"
               "    acc)",
               words, ns);
}

} // namespace

void RunBackendShiftTests(TestResults& results) {
    TestConstShifts8(results);
    TestCountShifts(results);
}

} // namespace tisple_tests
