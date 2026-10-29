// 16-bit runs of + - & | ^ a byte at a time through A (gen_spine.cpp), against the interpreter

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

void CheckPairs(TestResults& results, const std::string& text) {
    std::vector<std::uint16_t> xs = {0, 1, 0x00FF, 0x0100, 0x7FFF, 0x8000, 0x8001, 0xA5C3, 0xFFFF};
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

void TestSpines(TestResults& results) {
    const char* bodies[] = {
        "(& (- x y) 22133)",
        "(| (+ x 300) y)",
        "(^ (& x y) (+ y 0x1234))",
        "(- (& x 0x7F0F) (| y 7))",
        "(+ (^ (| x 0x00F0) y) 0x7001)",
        "(& (& (| x y) 0x7FFF) (^ x 0x5AA5))",
        // two adds: two carries, the old way
        "(& (+ (- x y) 5) 0x0FFF)",
        // the other operand read through an index
        "(& (- x (at w (& (as u8 y) 3))) (at w (& (as u8 x) 3)))",
    };
    for (const char* type : {"u16", "i16"}) {
        std::string t = type;
        for (const char* body : bodies) {
            CheckPairs(results, "(let w (array " + t + " 4) (make (array " + t +
                                    " 4) 1 300 5000 32767))\n"
                                    "(fn f ((x " +
                                    t + ") (y " + t + ")) " + t + " " + body + ")");
        }
    }
    // into a variable it reads, also a byte moved: written through a temporary
    CheckPairs(results, "(let g u16 0)\n"
                        "(fn f ((x u16) (y u16)) u16 (set g x) (set g (& (- g y) 0xF0F0)) g)");
    CheckPairs(results, "(let g u16 0)\n"
                        "(fn f ((x u16) (y u16)) u16 (set g x) (set g (| (- (<< g 8) y) 3)) g)");
    CheckPairs(results, "(let g u16 0)\n"
                        "(fn f ((x u16) (y u16)) u16 (set g x) (set g (^ (+ (>> g 8) y) g)) g)");
    // no temporary between the two
    Compiled c =
        CompileText("(let g u16 0)\n(fn f ((x u16) (y u16)) void (set g (& (- x y) 56901)))");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::STA) == 2);
}

} // namespace

void RunBackendSpineTests(TestResults& results) {
    TestSpines(results);
}

} // namespace tisple_tests
