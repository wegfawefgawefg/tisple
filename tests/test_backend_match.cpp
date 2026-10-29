// match dispatch: value tables for clauses that give numbers, statement clauses that do
// nothing going straight to the end (gen_match.cpp)

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

// f called with every byte against the interpreter
void CheckEveryByte(TestResults& results, const std::string& text, const std::string& function) {
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c);
    for (std::uint16_t x = 0; x < 256 && ok; x++) {
        ok = DiffCall(d, function, {x});
        if (!ok) {
            std::fprintf(stderr, "x = %u\n", x);
        }
    }
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

bool HasLabelIndexedLoad(const Compiled& c, const std::string& function) {
    return CountMn(c, function, tisple::Mn::PHA) == 0 && CountMn(c, function, tisple::Mn::LDA) > 0;
}

void TestValueTables(TestResults& results) {
    // the table runs from 0 and needs a compare
    CheckEveryByte(results, "(fn f ((x u8)) u8 (match x (2 10) (3 20) (5 30) (_ 99)))", "t.f");
    // the scrutinee's range is the table's: no compare
    std::string masked = "(fn f ((x u8)) u8 (match (& x 7) (0 1) (4 255) (6 128) (_ 7)))";
    CheckEveryByte(results, masked, "t.f");
    Compiled c = CompileText(masked);
    CHECK(results, CountMn(c, "t.f", tisple::Mn::CMP) == 0);
    CHECK(results, CountMn(c, "t.f", tisple::Mn::TAX) == 1);
    // cases far from 0: the scrutinee less the first first
    CheckEveryByte(results, "(fn f ((x u8)) u8 (match x (100 1) (101 2) (103 3) (_ 4)))", "t.f");
    CheckEveryByte(results, "(fn f ((x u8)) u8 (match x (250 1) (255 2) (_ 3)))", "t.f");
    // a small range that starts below the first case: still a compare
    CheckEveryByte(results, "(fn f ((x u8)) u8 (match (& x 15) (9 1) (10 2) (12 3) (_ 4)))", "t.f");
    // 16-bit values, into a global and returned
    CheckEveryByte(results,
                   "(let out u16 0)\n"
                   "(fn f ((x u8)) u16\n"
                   "    (set out (match (& x 15) (0 1000) (3 0xFFFF) (9 5) (_ 0x1234))) out)",
                   "t.f");
    CheckEveryByte(
        results,
        "(let out u16 0)\n"
        "(fn f ((x u8)) u16 (set out (match x (40 0x8000) (44 1) (47 0x00FF) (_ 2))) out)",
        "t.f");
    // signed numbers and an enum scrutinee; without a wildcard the values no clause names
    // give 0
    std::string e =
        "(enum E (a b c d e))\n"
        "(fn pick ((x u8)) E\n"
        "    (if (< x 64) E.a (if (< x 128) E.b (if (< x 200) E.c (if (< x 250) E.d E.e)))))\n";
    CheckEveryByte(results, e + "(fn f ((x u8)) i8 (match (pick x) (E.a -1) (E.c -128) (_ 127)))",
                   "t.f");
    CheckEveryByte(
        results,
        e + "(fn f ((x u8)) i8 (match (pick x) (E.a -1) (E.b 0) (E.c -128) (E.d 3) (E.e 9)))",
        "t.f");
    Compiled t = CompileText("(fn f ((x u8)) u8 (match x (2 10) (3 20) (5 30) (_ 1)))");
    CHECK(results, HasLabelIndexedLoad(t, "t.f"));
}

void TestValueTableInLoop(TestResults& results) {
    // the loop's counter is in X: the table is read through Y
    std::string text = "(let acc u16 0)\n"
                       "(fn f ((n u8)) u16\n"
                       "    (set acc 0)\n"
                       "    (for i u8 0 n\n"
                       "        (let v u8 (match (& i 3) (0 5) (1 7) (_ 9)))\n"
                       "        (set acc (+ acc (as u16 v))))\n"
                       "    acc)";
    CheckEveryByte(results, text, "t.f");
    Compiled c = CompileText(text);
    CHECK(results, CountMn(c, "t.f", tisple::Mn::TAY) == 1);
}

void TestIdleClauses(TestResults& results) {
    // clauses that do nothing go to the end, with and without a wildcard
    CheckEveryByte(results,
                   "(let g u8 0)\n"
                   "(fn f ((x u8)) u8\n"
                   "    (set g 1)\n"
                   "    (match x (1 (do)) (2 (set g 5)) (3 g) (7 (set g (+ g x))) (_ (do)))\n"
                   "    g)",
                   "t.f");
    CheckEveryByte(results,
                   "(let g u8 0)\n"
                   "(fn f ((x u8)) u8\n"
                   "    (set g 1)\n"
                   "    (match (& x 3) (0 (set g 9)) (1) (2 (do)) (_ (set g (- g x))))\n"
                   "    g)",
                   "t.f");
    // nothing to do at all: only the scrutinee's call is left
    std::string text = "(let g u8 0)\n"
                       "(fn h ((x u8)) u8 (set g (+ g x)) g)\n"
                       "(fn f ((x u8)) u8 (match (h x) (1 (do)) (2) (_ g)) g)";
    CheckEveryByte(results, text, "t.f");
    Compiled c = CompileText(text);
    CHECK(results, CountMn(c, "t.f", tisple::Mn::CMP) == 0);
    // an idle clause between others
    Compiled d = CompileText("(let g u8 0)\n"
                             "(fn f ((x u8)) void (match x (1 (set g 1)) (2) (3 (set g 3)) (_)))");
    CHECK(results, CountMn(d, "t.f", tisple::Mn::CMP) == 2);
}

} // namespace

void RunBackendMatchTests(TestResults& results) {
    TestValueTables(results);
    TestValueTableInLoop(results);
    TestIdleClauses(results);
}

} // namespace tisple_tests
