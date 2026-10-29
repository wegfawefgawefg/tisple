// a division routine's operands worked out straight into rt_a and rt_b (gen_div.cpp): only
// when the other operand cannot call a routine that sets them, so operands with divisions,
// multiplies and calls of their own go through temporaries as before

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

void CheckPairs(TestResults& results, const std::string& text) {
    std::vector<std::uint16_t> xs = {0,    1,    2,    3,    7,      0x7F,   0x80,  0x81,
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
    const char* sides[] = {"x", "(+ x 3)", "(^ x (>> y 2))", "(/ x (| y 1))", "(* x y)",
                           "(h x y)"};
    for (const char* type : {"u8", "i8", "u16", "i16"}) {
        std::string t = type;
        for (const char* a : sides) {
            for (const char* b : sides) {
                for (const char* op : {"/", "%"}) {
                    std::string text = "(let k " + t + " 0)\n(fn h ((x " + t + ") (y " + t +
                                       ")) " + t + " (set k (+ k 1)) (- x y))\n(fn f ((x " +
                                       t + ") (y " + t + ")) " + t + " (" + op + " " + a +
                                       " " + std::string(b) + "))";
                    CheckPairs(results, text);
                }
            }
        }
    }
    // a / and a % of the same worked-out operands still divide once
    CheckPairs(results, "(let r u16 0)\n(fn f ((x u16) (y u16)) u16\n"
                        "    (set r (% (+ x 1) (| y 3))) (+ r (/ (+ x 1) (| y 3))))");
}

void TestNoCopies(TestResults& results) {
    // the divisor's bytes are stored once, into rt_b, not into a temporary first
    Compiled w = CompileText("(fn f ((x u16) (y u16)) u16 (/ (+ x 1) (| y 1)))");
    CHECK(results, CountMn(w, "t.f", tisple::Mn::STA) <= 6);
    Compiled b = CompileText("(fn f ((x u8) (y u8)) u8 (/ (+ x 1) (| y 1)))");
    CHECK(results, CountMn(b, "t.f", tisple::Mn::STA) == 2);
}

} // namespace

void RunBackendRtArgsTests(TestResults& results) {
    TestShapes(results);
    TestNoCopies(results);
}

} // namespace tisple_tests
