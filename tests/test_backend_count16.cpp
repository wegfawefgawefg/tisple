// u16 for loops with constant bounds whose counter nothing reads count their trips down in a
// byte, as u8 ones do (gen_loop.cpp, GenFor), or run as copies of their body

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

void CheckRun(TestResults& results, const std::string& text, std::uint16_t x) {
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c) && DiffCall(d, "t.f", {x});
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  x = %u: %s\n", text.c_str(), x, d.message.c_str());
    }
    CHECK(results, ok);
}

void TestCountedWordLoops(TestResults& results) {
    // trips 1, 2, 7, 16, 17, 255, 256 and 257 (past a byte: counted the old way), across
    // page starts and the top of the range
    const char* bounds[][2] = {
        {"5", "6"},   {"254", "256"}, {"65533", "65536"}, {"250", "266"}, {"1000", "1017"},
        {"0", "255"}, {"100", "356"}, {"65280", "65536"}, {"7", "264"},   {"65535", "65536"}};
    for (const auto& b : bounds) {
        std::string head = std::string("(for n u16 ") + b[0] + " " + b[1];
        CheckRun(results,
                 "(let g u16 0)\n(let h u8 0)\n"
                 "(fn f ((x u8)) u16 (set g 0) (set h x)\n" +
                     head + " (set g (+ g (as u16 h))) (set h (^ h (<< h 1))))\n    g)",
                 0x35);
        // with a break and a continue
        CheckRun(results,
                 "(let g u16 0)\n"
                 "(fn f ((x u8)) u16 (set g 0)\n" +
                     head +
                     " (set g (+ g 3)) (if (= (& g 0xFF) (as u16 x)) (break))"
                     " (if (= (& g 1) 0) (continue)) (set g (+ g 1)))\n    g)",
                 0x40);
        // nested: the inner loop counted too
        CheckRun(results,
                 "(let g u16 0)\n"
                 "(fn f ((x u8)) u16 (set g (as u16 x))\n" +
                     head + " (for m u16 300 303 (set g (+ g (>> g 3)))))\n    g)",
                 9);
    }
    // a byte counter instead of the word one
    Compiled c =
        CompileText("(let g u8 0)\n(fn f ((x u8)) void (for n u16 1000 1100 (set g (+ g x))))");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::DEX) + CountMn(c, "t.f", tisple::Mn::DEY) +
                           CountMn(c, "t.f", tisple::Mn::DEC) ==
                       1);
    // a counter the body reads stays a word
    CheckRun(results,
             "(let g u16 0)\n(fn f ((x u8)) u16 (set g 0) (for n u16 250 260 (set g (+ g n))) g)",
             0);
}

} // namespace

void RunBackendCount16Tests(TestResults& results) {
    TestCountedWordLoops(results);
}

} // namespace tisple_tests
