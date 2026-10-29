// compile-time evaluation (src/opt/eval.cpp): calls of pure functions with constant arguments
// become their values; calls that must stay (effects, reads of globals or through pointers,
// loops that may not end, work over the step budget); each program also run on the 6502
// against the interpreter, which runs it as written

#include "test_backend.hpp"
#include "test_opt.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

using tisple::ExprKind;

const char* const kGlobals = "(let g u8 0)\n(let gw u16 0)\n(let a (array u8 8))\n"
                             "(let scr u8 :address 0x0400)\n"
                             "(const KT (array u16 4) (make (array u16 4) 4406 2 23778 65535))\n";

std::string With(const std::string& text) {
    return std::string(kGlobals) + text;
}

void ExpectCalls(TestResults& results, const std::string& text, int want) {
    Opted o = Optimize(text);
    int got = CountKind(o, "t.f", ExprKind::Call);
    if (got != want) {
        std::fprintf(stderr, "t.f makes %d calls, not %d:\n%s\n", got, want, text.c_str());
    }
    CHECK(results, o.ok && got == want);
}

void WorkedOut(TestResults& results) {
    const char* folded[] = {
        // a loop
        "(fn tri ((n u8)) u16 (let s u16 0) (for i u8 0 n (set s (+ s (as u16 i)))) s)\n"
        "(fn f ((x u8)) u16 (+ (tri 20) (as u16 x)))",
        // a const table, a call inside, signed wrapping, an enum and a bool
        "(fn pick ((k u8)) u16 (at KT (& k 3)))\n"
        "(fn twice ((v u16)) u16 (+ (pick 2) v))\n"
        "(fn f ((x u8)) u16 (+ (twice 50000) (as u16 x)))",
        "(fn neg ((v i8)) i8 (- 0 (* v 3)))\n(fn f ((x u8)) u8 (as u8 (+ (neg -50) (as i8 x))))",
        "(fn odd ((v u8)) bool (= (& v 1) 1))\n(fn f ((x u8)) u8 (if (odd 7) x 0))",
        // one call's value is the next one's argument
        "(fn sq ((v u8)) u8 (* v v))\n(fn f ((x u8)) u8 (+ (sq (sq 3)) x))",
        // a while that provably ends
        "(fn steps ((v u8)) u8 (let c u8 0) (let n u8 v) (while (< n 200) (set n (+ n 7))\n"
        "    (set c (+ c 1))) c)\n"
        "(fn f ((x u8)) u8 (+ (steps 3) x))",
    };
    for (const char* text : folded) {
        ExpectCalls(results, With(text), 0);
        CheckProgram(results, With(text), "t.f", {9});
    }
}

void Stays(TestResults& results) {
    // must stay calls: arguments not constant, a global written, a global read (its value
    // then is not its value now), a load through a pointer, io_read, io_write, jsr,
    // wait_cycles, an :address read, a while that may not end, work past the step budget
    const char* kept[] = {
        "(fn sq ((v u8)) u8 (* v v))\n(fn f ((x u8)) u8 (sq x))",
        "(fn put ((v u8)) u8 (set g v) v)\n(fn f ((x u8)) u8 (+ (put 4) x))",
        "(fn rd ((v u8)) u8 (+ g v))\n(fn f ((x u8)) u8 (set g x) (rd 4))",
        "(fn ld ((v u8)) u8 (let p (ptr u8) (addr_of (at a 1))) (+ (load p) v))\n"
        "(fn f ((x u8)) u8 (set (at a 1) x) (ld 4))",
        "(fn key ((v u8)) u8 (+ (io_read u8 0xC000) v))\n(fn f ((x u8)) u8 (+ (key 4) x))",
        "(fn click ((v u8)) u8 (io_write u8 0xC030 v) v)\n(fn f ((x u8)) u8 (+ (click 4) x))",
        "(fn hw ((v u8)) u8 (+ scr v))\n(fn f ((x u8)) u8 (set scr x) (hw 4))",
        "(fn pause ((v u8)) u8 (wait_cycles 100) v)\n(fn f ((x u8)) u8 (+ (pause 4) x))",
        "(fn spin ((v u8)) u8 (let c u8 v) (while (!= c 0) (set c (- c 2))) c)\n"
        "(fn f ((x u8)) u8 (+ (spin 4) x))",
        "(fn long ((v u16)) u16 (let s u16 0) (for i u16 0 60000 (set s (+ s v))) s)\n"
        "(fn f ((x u8)) u16 (+ (long 3) (as u16 x)))",
    };
    for (const char* text : kept) {
        ExpectCalls(results, With(text), 1);
        CheckProgram(results, With(text), "t.f", {9});
    }
    // machine code: the interpreter cannot run it, now or later
    ExpectCalls(results,
                With("(fn bell ((v u8)) u8 (jsr 0xFBDD) v)\n(fn f ((x u8)) u8 (+ (bell 4) x))"), 1);
}

} // namespace

void RunOptEvalTests(TestResults& results) {
    WorkedOut(results);
    Stays(results);
}

} // namespace tisple_tests
