// signed compares of values that are never negative are done unsigned (NeverNegative), in
// conditions and in a for loop's guard

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

void CheckPairs(TestResults& results, const std::string& text) {
    std::vector<std::uint16_t> xs = {0,    1,    2,    0x3F, 0x40,  0x7E,   0x7F,   0x80,
                                     0x81, 0xC0, 0xFE, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF};
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

void TestNeverNegative(TestResults& results) {
    for (const char* op : {"<", "<=", ">", ">="}) {
        std::string o = op;
        CheckPairs(results,
                   "(fn f ((x u8) (y u8)) bool (" + o + " (as i8 (& x 0x7F)) (as i8 (>> y 1))))");
        CheckPairs(results, "(fn f ((x u8) (y u8)) bool (" + o + " (as i8 (& x 0x7F)) 100))");
        // these may be negative: still signed
        CheckPairs(results,
                   "(fn f ((x u8) (y u8)) bool (" + o + " (as i8 (& x 0xFF)) (as i8 (>> y 1))))");
        CheckPairs(results,
                   "(fn f ((x u8) (y u8)) bool (" + o + " (as i8 (& x 0x80)) (as i8 (>> y 1))))");
        CheckPairs(results, "(fn f ((x u8) (y u8)) bool (" + o + " (as i8 (& x 0x7F)) -3))");
        CheckPairs(results, "(fn f ((x u16) (y u16)) bool (" + o +
                                " (as i16 (& x 0x7FFF)) (as i16 (>> y 1))))");
        CheckPairs(results, "(fn f ((x u16) (y u16)) bool (" + o +
                                " (as i16 (& x 0x80FF)) (as i16 (>> y 1))))");
    }
    std::string text = "(fn f ((x u8) (y u8)) bool (< (as i8 (& x 0x7F)) (as i8 (>> y 1))))";
    Compiled c = CompileText(text);
    CHECK(results, CountMn(c, "t.f", tisple::Mn::BVC) == 0);
    // a for loop's guard
    CheckPairs(results, "(let n u8 0)\n"
                        "(fn f ((x u8) (y u8)) u8\n"
                        "    (set n 0)\n"
                        "    (for i i8 3 (as i8 (& x 7)) (set n (+ n 1)))\n"
                        "    (for i i8 (as i8 (& y 0x3F)) (as i8 (& x 0x7F)) (set n (+ n 2)))\n"
                        "    (for i i8 -2 (as i8 (& x 7)) (set n (+ n 3)))\n"
                        "    n)");
    Compiled loop =
        CompileText("(let n u8 0)\n"
                    "(fn f ((x u8)) void (for i i8 3 (as i8 (& x 7)) (set n (+ n 1))))");
    CHECK(results, CountMn(loop, "t.f", tisple::Mn::BVC) == 0);
}

// a constant at the end of its type's range decides an order compare; the other side's
// effects still happen
void TestDecided(TestResults& results) {
    struct Type {
        const char* name;
        const char* min;
        const char* max;
    };
    const Type types[] = {{"u8", "0", "255"},
                          {"i8", "-128", "127"},
                          {"u16", "0", "65535"},
                          {"i16", "-32768", "32767"}};
    for (const Type& t : types) {
        std::string n = t.name;
        for (const char* op : {"<", "<=", ">", ">="}) {
            for (const char* c : {t.min, t.max}) {
                std::string text = "(let g " + n +
                                   " 0)\n(let k u8 0)\n"
                                   "(fn h ((x u8)) " +
                                   n + " (set k (+ k 1)) (set g (as " + n +
                                   " (* x 7))) g)\n"
                                   "(fn f ((x u8) (y u8)) u8 (set k 0)\n"
                                   "    (if (" +
                                   op + " (h x) " + c +
                                   ") (set k (+ k 10)))\n"
                                   "    (if (" +
                                   op + " " + c +
                                   " (h y)) (set k (+ k 20)))\n"
                                   "    k)";
                CheckPairs(results, text);
            }
        }
    }
    Compiled c = CompileText("(let g u16 0)\n(fn f ((x u16)) u8 (if (>= 65535 x) 1 2))");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::CMP) == 0);
}

} // namespace

void RunBackendSignedCmpTests(TestResults& results) {
    TestNeverNegative(results);
    TestDecided(results);
}

} // namespace tisple_tests
