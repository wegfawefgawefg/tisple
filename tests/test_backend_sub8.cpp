// 8-bit subtractions whose right side needs code: a - b as ~b + 1 + a when a is read where it
// is, b first when neither side has effects (gen_arith.cpp, EvalArith8)

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

void CheckPairs(TestResults& results, const std::string& text) {
    std::vector<std::uint16_t> xs = {0, 1, 2, 0x55, 0x7F, 0x80, 0x81, 0xC3, 0xFE, 0xFF};
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

void TestSubOrder(TestResults& results) {
    // a read where it is, b with code
    std::string simple = "(fn f ((x u8) (y u8)) u8 (- x (<< (^ y x) 1)))";
    CheckPairs(results, simple);
    Compiled c = CompileText(simple);
    CHECK(results, CountMn(c, "t.f", tisple::Mn::SBC) == 0);
    // a constant on the left
    CheckPairs(results, "(fn f ((x u8) (y u8)) u8 (- 200 (& (+ y x) 0x3C)))");
    // both with code and no effects: b first
    std::string both = "(fn f ((x u8) (y u8)) u8 (- (| (>> x 1) y) (<< (^ y x) 1)))";
    CheckPairs(results, both);
    Compiled d = CompileText(both);
    CHECK(results, CountMn(d, "t.f", tisple::Mn::STA) == 1);
    // b writes what a reads: a is read before b runs
    CheckPairs(results, "(let g u8 0)\n"
                        "(fn h ((v u8)) u8 (set g (+ g v)) (^ g 0x0F))\n"
                        "(fn f ((x u8) (y u8)) u8 (set g x) (- g (h y)))");
    // a has effects: left to right
    CheckPairs(results, "(let g u8 0)\n"
                        "(fn h ((v u8)) u8 (set g (+ g v)) (^ g 0x0F))\n"
                        "(fn f ((x u8) (y u8)) u8 (set g x) (- (h y) (+ g (<< y 1))))");
}

} // namespace

void RunBackendSub8Tests(TestResults& results) {
    TestSubOrder(results);
}

} // namespace tisple_tests
