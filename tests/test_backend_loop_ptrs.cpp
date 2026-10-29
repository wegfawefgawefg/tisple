// the zero-page loop pointers a loop hands out are free again after it (gen_stmt.cpp): a
// function of many loops in a row fits, and loops after a page pointer leave it alone

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

void TestManyLoops(TestResults& results) {
    // each loop reads and writes through derived pointers ((ptr),Y with the counter's low
    // byte in Y): 4 bytes a loop, 280 for all of them if none were given back, more than zero
    // page has
    std::string text = "(let wb (array u8 1024))\n(let ww (array u16 512))\n"
                       "(fn f ((x u16)) u16\n";
    for (int k = 0; k < 70; k++) {
        std::string a = std::to_string(k * 7 % 400);
        std::string b = std::to_string(k * 13 % 300);
        text += "    (for i u16 (& x 7) 250 (set (at wb (+ i " + a + ")) (+ (at wb (+ i " + b +
                ")) " + std::to_string(k) + ")))\n";
    }
    text += "    (+ (at ww 3) (as u16 (at wb 17))))";
    Compiled c = CompileText(text);
    CHECK(results, c.ok);
    if (!c.ok) {
        std::fprintf(stderr, "%s\n", c.errors.c_str());
        return;
    }
    Diff d;
    bool ok = StartDiff(d, c);
    for (int x : {0, 3, 7}) {
        ok = ok && DiffCall(d, "t.f", {static_cast<std::uint16_t>(x)});
    }
    if (!ok) {
        std::fprintf(stderr, "%s\n", d.message.c_str());
    }
    CHECK(results, ok);
}

// a page pointer's low byte is set once at the function's start: no earlier loop may have
// used its bytes (farm programs 7001713991 and others in run6 cover the shapes)
void TestPagePointerAfterLoops(TestResults& results) {
    std::string text = "(let wb (array u8 1024))\n(let n u16 0)\n"
                       "(fn f ((x u16)) u16\n"
                       "    (for i u16 (& x 7) 200 (set (at wb (+ i 9)) (at wb (+ i 300))))\n"
                       "    (set n (+ (& x 3) 100))\n"
                       "    (let s u16 0)\n"
                       "    (while (< n 600) (set s (+ s (as u16 (at wb n)))) (set n (+ n 1)))\n"
                       "    s)";
    // and a loop after the one that made it, both inside another, runs between its uses
    text += "\n(fn g ((x u16)) u16\n"
            "    (let s u16 0)\n"
            "    (for o u8 0 3\n"
            "        (set n (+ (& x 3) (as u16 (* o 50))))\n"
            "        (while (< n 600) (set s (+ s (as u16 (at wb n)))) (set n (+ n 1)))\n"
            "        (for i u16 (& x 7) 200 (set (at wb (+ i 9)) (+ (at wb (+ i 300)) o))))\n"
            "    s)";
    // an outer loop's pointers made after an inner loop gave its back are set before the
    // outer loop starts: they must not take the inner one's bytes (race seed 342)
    text += "\n(let ww (array u16 512))\n"
            "(fn h ((x u16)) u16\n"
            "    (for n u16 250 256\n"
            "        (set (at ww n) (+ n x))\n"
            "        (let v u16 (+ (& x 7) 300)) (let k u8 0)\n"
            "        (while (< k 6) (set (at wb v) (+ (at wb v) k)) (set v (+ v 1)) (set k (+ k "
            "1)))\n"
            "        (set (at wb n) (as u8 v)))\n"
            "    (+ (at ww 251) (as u16 (at wb 302))))";
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c);
    for (int x : {0, 1, 5, 7}) {
        ok = ok && DiffCall(d, "t.h", {static_cast<std::uint16_t>(x)});
        ok = ok && DiffCall(d, "t.g", {static_cast<std::uint16_t>(x)});
        ok = ok && DiffCall(d, "t.f", {static_cast<std::uint16_t>(x)});
    }
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

} // namespace

void RunBackendLoopPtrsTests(TestResults& results) {
    TestManyLoops(results);
    TestPagePointerAfterLoops(results);
}

} // namespace tisple_tests
