// slice 5's ranges (src/opt/ranges_vars.cpp, ranges.cpp, ranges_elements.cpp,
// collapse_continue.cpp, consts_eval.cpp): what each variable may ever hold (arguments every
// call passes, steps that wrap), what a set gives the items after it, elements no store
// reaches, (if c (continue)) items, (+ x (~ x)), a bool tested false, short loops unrolled
// (src/opt/unroll.cpp); what they must not
// decide. Every program has a main (with none, any function may be called from outside) and
// also runs from it on the 6502 against the interpreter, which runs it as written

#include "test_backend.hpp"
#include "test_opt.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

using tisple::ExprKind;

const char* const kGlobals =
    "(enum E (e0 e1 e2))\n(let g u8 0)\n(let h u8 0)\n(let gw u16 0)\n(let e E E.e0)\n"
    "(let flag bool false)\n(let t (array u8 8) (make (array u8 8) 1 2 3 4 5 6 7 8))\n"
    "(let other (array u8 4))\n(let sv u16 65535)\n(let sb u8 255)\n"
    // what main sets h from, which nothing can know
    "(let scr u8 :address 0x0400)\n"
    "(fn bump () void (set g (+ g 3)))\n";

std::string With(const std::string& text) {
    return std::string(kGlobals) + text;
}

int Branches(const Opted& o) {
    return CountKind(o, "t.f", ExprKind::If) + CountKind(o, "t.f", ExprKind::Match) +
           CountKind(o, "t.f", ExprKind::For) + CountKind(o, "t.f", ExprKind::While);
}

struct Case {
    const char* text;
    int branches; // decided: exactly this many ifs, matches and loops left in t.f; kept: as many
                  // or more
};

void Expect(TestResults& results, const Case& c, bool decided) {
    Opted o = Optimize(With(c.text));
    int got = Branches(o);
    bool fits = decided ? got == c.branches : got >= c.branches;
    if (!fits) {
        std::fprintf(stderr, "%d branches and loops left in t.f, not %s%d:\n%s\n", got,
                     decided ? "" : "at least ", c.branches, c.text);
    }
    CHECK(results, o.ok && fits);
    if (std::string(c.text).find("(fn main") != std::string::npos) {
        CheckProgram(results, With(c.text), "t.main", {});
    }
}

void Decided(TestResults& results) {
    const Case decided[] = {
        // every call passes 65535 for x, and y is 3 or 9
        {"(fn f ((x u16) (y u16)) void (if (>= y x) (set g 1)))\n"
         "(fn main () void (f 65535 3) (f 65535 9))",
         0},
        // sv is 65535 or 1: 50 steps keep v in 950..1050, so w is 64486..64586
        {"(fn f () void (let v u16 1000) (let c u8 0)\n"
         "    (while (< c 50) (set c (+ c 1)) (set v (+ v sv))\n"
         "        (let w u16 (- v 2000)) (if (< w 60000) (set g 1)))\n"
         "    (set gw v))\n"
         "(fn main () void (if (> h 3) (set sv 1)) (f))",
         0},
        // a step taken off: (- v sb) for sb 255 or 1, -1 or 1
        {"(fn f () void (let v u8 100) (let c u8 0)\n"
         "    (while (< c 20) (set c (+ c 1)) (set v (- v sb)) (if (> v 130) (set g 1))))\n"
         "(fn main () void (if (= h 0) (set sb 1)) (f))",
         0},
        // (% g 9) then (/ g 9): g is 0 after them
        {"(fn f () void (set g (% g 9)) (set g (/ g 9)) (if (!= g 0) (set h 1)))\n"
         "(fn main () void (set g 200) (f))",
         0},
        // (| x 1) for x in 0..8 is at most 9: the remainder is below 9
        {"(fn f () void (set g (% (% h (| g 1)) 9)) (set h (/ (% h (| g 1)) 9))\n"
         "    (if (!= h 0) (set g 1)))\n"
         "(fn main () void (set h scr) (set g scr) (f))",
         0},
        // an element no store reaches: (at t 5) is 6
        {"(fn f () void (set (at t 2) g) (if (> (at t 5) 6) (set h 1)))\n(fn main () void (f))", 0},
        // stores at indexes 0..3 only; (at t (+ 4 (& h 3))) is 5..8
        {"(fn f () void (set (at t (& g 3)) 0) (if (< (at t (+ 4 (& h 3))) 5) (set h 1)))\n"
         "(fn main () void (f))",
         0},
        // e only ever holds e0 or e1
        {"(fn f () void (if (= e E.e2) (set g 1)))\n(fn main () void (set e E.e1) (f))", 0},
        // a function nothing that runs calls stores nothing: (at other 1) is 0
        {"(fn unused () void (set (at other 1) 9))\n"
         "(fn f () void (if (!= (at other 1) 0) (set g 1)))\n(fn main () void (f))",
         0},
        // a continue item becomes an else; then the passes all do the same (the else stays)
        {"(fn f () void (for i u8 0 4 (set g 7) (if (> h 2) (continue)) (set (at other 1) 3)))\n"
         "(fn main () void (set h scr) (f))",
         1},
        // x + ~x is all ones
        {"(fn f () void (set g (+ h (~ h))) (if (!= g 255) (set h 1)))\n(fn main () void (f))", 0},
        // after (if flag (return 1)), flag is false
        {"(fn f () u8 (if flag (return 1)) (if flag (set g 1)) 0)\n"
         "(fn main () void (set flag (> h 3)) (set g (f)))",
         1},
        // short loops whose passes depend on the one before, unrolled and folded: 27, 18, and
        // -1 from the pass that ors it in
        {"(fn f () void (let v u8 1) (for i u8 0 4 (set v (+ (* v 2) i))) (if (!= v 27) (set h "
         "1)))\n"
         "(fn main () void (f))",
         0},
        {"(fn f () void (let c u8 0) (let v u8 3) (while (< c 3) (set c (+ c 1)) (set v (* v c)))\n"
         "    (if (!= v 18) (set h 1)))\n"
         "(fn main () void (f))",
         0},
        // passes not known: b - a of them, worked out before the loop (no guard: -4 is below
        // whatever (& h 7) is)
        {"(fn f () void (for i i8 -4 (as i8 (& h 7)) (set gw (+ gw 3)) (set g (- g 1))))\n"
         "(fn main () void (set h scr) (f))",
         0},
        {"(fn f () void (let v i8 (as i8 scr)) (for i i8 -3 4 (set v (| i v)))\n"
         "    (if (!= v -1) (set h 1)))\n"
         "(fn main () void (f))",
         0},
    };
    for (const Case& c : decided) {
        Expect(results, c, true);
    }
}

void Kept(TestResults& results) {
    const Case kept[] = {
        // the calls pass different x
        {"(fn f ((x u16) (y u16)) void (if (>= y x) (set g 1)))\n"
         "(fn main () void (f 65535 3) (f 7 9))",
         1},
        // no main: f may be called from outside with anything
        {"(fn f ((x u16) (y u16)) void (if (>= y x) (set g 1)))\n(fn k () void (f 65535 3))", 1},
        // a step in a loop inside: three a pass
        {"(fn f () void (let v u16 1000) (let c u8 0) (while (< c 50) (set c (+ c 1))\n"
         "        (for k u8 0 3 (set v (+ v sv))) (if (< v 900) (set g 1)))\n"
         "    (set gw v))\n"
         "(fn main () void (if (> h 3) (set sv 1)) (f))",
         2},
        // a write that is no step
        {"(fn f () void (let v u8 100) (let c u8 0) (while (< c 20) (set c (+ c 1))\n"
         "    (set v (+ v 1)) (if (> h 3) (set v 0)) (if (< v 90) (set g 1))))\n"
         "(fn main () void (set h 5) (f))",
         3},
        // a call between writes g
        {"(fn f () void (set g (% g 9)) (bump) (set g (/ g 9)) (if (!= g 0) (set h 1)))\n"
         "(fn main () void (set g 200) (f))",
         1},
        // a loop between writes g
        {"(fn f () void (set g (% g 9)) (for i u8 0 (& h 3) (set g (+ g 5))) (set g (/ g 9))\n"
         "    (if (!= g 0) (set h 1)))\n"
         "(fn main () void (set g 200) (f))",
         2},
        // stored through a pointer
        {"(fn f () void (store (addr_of (at t 5)) 99) (if (> (at t 5) 6) (set h 1)))\n"
         "(fn main () void (f))",
         1},
        // a masked index reaches it
        {"(fn f () void (set (at t (& h 7)) 9) (if (> (at t 5) 6) (set g 1)))\n(fn main () void "
         "(set h scr) (f))",
         1},
        // a store whose index may fall outside its array: nothing is known of any element
        {"(fn f () void (set (at other (& h 7)) 9) (if (> (at t 5) 6) (set g 1)))\n"
         "(fn main () void (set h scr) (f))",
         1},
        // e may hold e2
        {"(fn f () void (if (= e E.e2) (set g 1)))\n(fn main () void (set e E.e2) (f))", 1},
        // a continue in an arm stays: the loop does not collapse
        {"(fn f () void (for i u8 0 4 (set g 7) (if (> h 2) (do (set gw 1) (continue)))\n"
         "    (set (at other 1) 3)))\n"
         "(fn main () void (set h scr) (f))",
         2},
        // x + ~y is not all ones
        {"(fn f () void (set g (+ h (~ g))) (if (!= g 255) (set h 1)))\n(fn main () void (f))", 1},
        // a bool tested true is not known to be 1
        {"(fn f () u8 (if flag (if flag (set g 1))) 0)\n(fn main () void (set flag true) (f))", 2},
        // passes not known, and the bound read after the loop writes it, or ^ that must know
        // whether they are odd
        {"(fn f () void (for i i8 -4 (as i8 (& h 7)) (set gw (+ gw 3)) (set h 1)))\n"
         "(fn main () void (set h scr) (f))",
         1},
        {"(fn f () void (for i i8 -4 (as i8 (& h 7)) (set g (^ g 5))))\n(fn main () void (set h "
         "scr) (f))",
         1},
        // loops not unrolled: nine passes; a call, I/O or a break in the body
        {"(fn f () void (let v u8 1) (for i u8 0 9 (set v (+ (* v 2) i))) (set h v))\n"
         "(fn main () void (f))",
         1},
        {"(fn f () void (let v u8 1) (for i u8 0 4 (set v (+ (* v 2) i)) (bump)) (set h v))\n"
         "(fn main () void (f))",
         1},
        {"(fn f () void (let v u8 1) (for i u8 0 4 (set v (+ (* v 2) i)) (io_write u8 0xC030 v))\n"
         "    (set h v))",
         1},
        {"(fn f () void (let v u8 1) (for i u8 0 4 (set v (+ (* v 2) i)) (if (> v scr) (break)))\n"
         "    (set h v))\n"
         "(fn main () void (f))",
         1},
    };
    for (const Case& c : kept) {
        Expect(results, c, false);
    }
}

} // namespace

void RunOptValuesTests(TestResults& results) {
    Decided(results);
    Kept(results);
}

} // namespace tisple_tests
