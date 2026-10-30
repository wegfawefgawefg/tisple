// the zero-page loop pointers a loop hands out are free again after it (gen_stmt.cpp): a
// function of many loops in a row fits, and loops after a page pointer leave it alone; an
// outer loop's step pointers follow a variable an inner loop kept in Y

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

// a page pointer made in a loop is read again where its function is inlined after a later
// loop: that loop may not take its bytes (fuzz seed 6063372, reduced)
void TestPagePointerInlinedAgain(TestResults& results) {
    std::string text =
        "(struct S ((a u8) (b i16) (c bool) (d u8)))\n"
        "(enum E (e0 e1 e2 e3 e4))\n"
        "(let g0 u8 99) (let g1 u8 128) (let g2 i8 -39) (let g3 u16 64578) (let g6 i16 32767)\n"
        "(let g10 E) (let arr13 (array i8 512)) (let pt (array u8 16))\n"
        "(let wb (array u8 1024)) (let sm (array u8 32)) (let sa (array S 8))\n"
        "(fn f0 () i16 g6)\n"
        "(fn f1 () void (for n19 u8 1 6 (do)))\n"
        "(fn f2 ((a20 u8) (a21 u16)) u8\n"
        "    (do (let n22 u16 (+ (as u16 (- a20 g1)) 277))\n"
        "        (while (and (< n22 113) (!= (at wb n22) a20)) (set n22 (+ n22 1)))\n"
        "        (set g3 (+ g3 n22)))\n"
        "    (- (+ (+ (at sm 3) (at sm (& (at sa (& g1 7) a) 31))) (+ (at sm 3) 102))\n"
        "       (load (+ \"Hello, \" (& g0 7)))))\n"
        "(fn f3 ((a23 i8) (a24 E)) u8\n"
        "    (set (at arr13 (& (+ g3 g3) 511)) a23)\n"
        "    (io_read u8 49155))\n"
        "(fn main () u8\n"
        "    (for n31 i8 -1 2 (f3 103 g10) (set g2 n31) (let l32 u8 255))\n"
        "    (store (+ (addr_of (at sm (& (f2 g1 g3) 15))) (as u16 (& (as u8 g2) 15))) 252)\n"
        "    (set g1 g0)\n"
        "    (f3 (- (~ g2) g2) g10))";
    Compiled c = CompileMainText(text);
    Diff d;
    bool ok = StartDiff(d, c) && DiffCall(d, "t.main", {});
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

// Hoare's partition (races/programs/sort): the outer loop steps i and j, so (at keys j) is
// a step pointer of it, and the inner whiles keep i and j in Y; the store after them went
// through the pointer of j from before the inner loop (quicksort never ended)
void TestStepPointerAfterInnerLoops(TestResults& results) {
    const char* text = "(let keys (array u16 300))\n"
                       "(let bytes (array u8 600))\n"
                       "(fn setup () void\n"
                       "    (for i u16 0 300 (set (at keys i) (& (* i 97) 0x3FF)))\n"
                       "    (for i u16 0 600 (set (at bytes i) (as u8 (* i 7)))))\n"
                       "(fn f ((hi u16)) u16\n"
                       "    (let i u16 0)\n"
                       "    (let j u16 hi)\n"
                       "    (while true\n"
                       "        (while (< (at keys i) 500)\n"
                       "            (set i (+ i 1)))\n"
                       "        (while (> (at keys j) 500)\n"
                       "            (set j (- j 1)))\n"
                       "        (if (>= i j)\n"
                       "            (return j))\n"
                       "        (set (at keys j) 0xFFFF)\n"
                       "        (set (at keys i) (+ (at keys i) 1))\n"
                       "        (set j (- j 1)))\n"
                       "    j)\n"
                       "(fn g ((lo u16) (hi u16)) u16\n"
                       "    (let i u16 lo)\n"
                       "    (let n u16 0)\n"
                       "    (while (< i hi)\n"
                       "        (while (and (< i hi) (< (at bytes i) 128))\n"
                       "            (set i (+ i 1)))\n"
                       "        (set (at bytes i) (+ (at bytes i) 1))\n"
                       "        (set n (+ n (as u16 (at bytes (+ i 1)))))\n"
                       "        (set i (+ i 3)))\n"
                       "    n)\n";
    for (int level = 0; level < 2; level++) {
        tisple::OptOptions opt;
        opt.level = level;
        Compiled c = CompileText(text, opt);
        Diff d;
        bool ok = StartDiff(d, c);
        for (int hi : {299, 298, 255, 256, 257, 100, 1}) {
            ok = ok && DiffCall(d, "t.setup", {}) &&
                 DiffCall(d, "t.f", {static_cast<std::uint16_t>(hi)});
        }
        for (int lo : {0, 1, 200, 254, 255, 256}) {
            ok = ok && DiffCall(d, "t.setup", {}) &&
                 DiffCall(d, "t.g", {static_cast<std::uint16_t>(lo), 598});
        }
        if (!ok) {
            std::fprintf(stderr, "step pointers, -O%d:\n%s\n  %s\n", level, text,
                         d.message.c_str());
        }
        CHECK(results, ok);
    }
}

} // namespace

void RunBackendLoopPtrsTests(TestResults& results) {
    TestStepPointerAfterInnerLoops(results);
    TestPagePointerInlinedAgain(results);
    TestManyLoops(results);
    TestPagePointerAfterLoops(results);
}

} // namespace tisple_tests
