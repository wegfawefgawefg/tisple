// loop-invariant code motion (src/opt/licm.cpp): what moves out of loops and how far, and
// what must stay because the loop (or a call in it) may change its inputs; each program also
// run on the 6502 against the interpreter, which runs it as written

#include "test_backend.hpp"
#include "test_opt.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

using tisple::ExprKind;

const char* const kGlobals = "(let g u8 0)\n(let h u8 0)\n(let gw u16 0)\n"
                             "(let a (array u8 8))\n(let b (array u8 8))\n"
                             "(let scr u8 :address 0x0400)\n";

std::string With(const std::string& text) {
    return std::string(kGlobals) + text;
}

// the passes but loop collapse, which would run most of these loops' bodies once
tisple::OptOptions NoCollapse() {
    tisple::OptOptions options;
    options.disabled = {"collapse"};
    return options;
}

constexpr int kDiv = static_cast<int>(tisple::Op::Div);
constexpr int kMod = static_cast<int>(tisple::Op::Mod);
constexpr int kMul = static_cast<int>(tisple::Op::Mul);
constexpr int kAdd = static_cast<int>(tisple::Op::Add);

// t.f's deepest binary of this op ends up this deep in loops
void ExpectDepth(TestResults& results, const std::string& text, int op, int want) {
    Opted o = Optimize(text, NoCollapse());
    int got = LoopDepth(o, "t.f", ExprKind::Binary, op);
    if (got != want) {
        std::fprintf(stderr, "op %d of t.f is %d loops in, not %d:\n%s\n", op, got, want,
                     text.c_str());
    }
    CHECK(results, o.ok && got == want);
}

void Moves(TestResults& results) {
    // out of a for, and out of a while's condition
    std::string basic = With("(fn f ((x u8) (y u8)) u8\n"
                             "    (for i u8 0 10 (set (at a (& i 7)) (/ x y))) (at a 3))");
    ExpectDepth(results, basic, kDiv, 0);
    CheckProgram(results, basic, "t.f", {200, 7});
    CheckProgram(results, basic, "t.f", {200, 0});
    std::string cond = With("(fn f ((x u16) (y u16)) u8 (let c u16 0)\n"
                            "    (while (< c (% x y)) (set c (+ c 1))) (set g (as u8 c)) g)");
    ExpectDepth(results, cond, kMod, 0);
    CheckProgram(results, cond, "t.f", {300, 7});
    // out of both loops at once
    std::string nested = With("(fn f ((x u16) (y u16)) u16\n"
                              "    (for i u8 0 3 (for j u8 0 3 (set gw (+ gw (* x y))))) gw)");
    ExpectDepth(results, nested, kMul, 0);
    ExpectDepth(results, nested, kAdd, 2);
    CheckProgram(results, nested, "t.f", {300, 7});
    // out of the inner loop only: the outer one changes i
    std::string inner = With("(fn f ((y u8)) u8\n"
                             "    (for i u8 1 5 (for j u8 0 3 (set (at a j) (/ i y)))) (at a 1))");
    ExpectDepth(results, inner, kDiv, 1);
    CheckProgram(results, inner, "t.f", {3});
    // the same expression twice is worked out once; a const array read and a pure call move
    std::string twice = With("(const KT (array u8 4) (make (array u8 4) 9 8 7 6))\n"
                             "(fn sq ((v u8)) u8 (* v v))\n"
                             "(fn f ((x u8) (y u8)) u8\n"
                             "    (for i u8 0 4 (set (at a i) (/ x y)) (set h (+ h (% x y)))\n"
                             "        (set (at b i) (/ x y)) (set g (+ g (sq (at KT (& x 3))))))\n"
                             "    (+ (at a 1) (at b 2)))");
    Opted o = Optimize(twice);
    CHECK(results, CountKind(o, "t.f", ExprKind::Call) == 1);
    CHECK(results, LoopDepth(o, "t.f", ExprKind::Call) == 0);
    ExpectDepth(results, twice, kDiv, 0);
    ExpectDepth(results, twice, kMod, 0);
    int divisions = 0;
    std::int32_t f = tisple::FindFunction(o.program, "t.f");
    for (const tisple::Expr& e : o.program.exprs) {
        divisions += e.kind == ExprKind::Binary && e.op == tisple::Op::Div ? 1 : 0;
    }
    CHECK(results, f >= 0 && divisions == 1);
    CheckProgram(results, twice, "t.f", {100, 7});
    // a loop that may run zero times: behind a guard that tests its range (or condition)
    // first; a divisor of zero gives the same results
    std::string zero = With("(fn f ((x u8) (y u8)) u8 (for i u8 0 x (set g (/ 200 y))) g)");
    ExpectDepth(results, zero, kDiv, 0);
    ExpectCount(results, zero, "t.f", ExprKind::If, 1);
    CheckProgram(results, zero, "t.f", {0, 0});
    CheckProgram(results, zero, "t.f", {3, 0});
    std::string range = With("(fn f ((x u8) (y u8)) u8\n"
                             "    (for i i8 (as i8 (- x 9)) (as i8 x) (set g (+ g (/ y x)))) g)");
    ExpectDepth(results, range, kDiv, 0);
    CheckProgram(results, range, "t.f", {3, 50});
    CheckProgram(results, range, "t.f", {200, 50});
    std::string again = With("(fn f ((x u8) (y u8)) u8 (let c u8 0)\n"
                             "    (while (< c x) (set c (+ c 1)) (set g (+ g (/ y x)))) g)");
    ExpectDepth(results, again, kDiv, 0);
    CheckProgram(results, again, "t.f", {0, 50});
    CheckProgram(results, again, "t.f", {4, 50});
    // a literal range that runs needs no guard
    std::string runs = With("(fn f ((x u8) (y u8)) u8 (for i u8 2 5 (set g (+ g (/ y x)))) g)");
    ExpectCount(results, runs, "t.f", ExprKind::If, 0);
    ExpectDepth(results, runs, kDiv, 0);
}

void Stays(TestResults& results) {
    // inputs the loop changes, or may: the counter, a let inside, a global it sets, a global a
    // call in it sets, an address-taken array stored through a pointer, an :address let, a
    // load through a pointer, a pure function reading a global the loop sets
    const char* kept[] = {
        "(fn f ((y u8)) u8 (for i u8 1 5 (set g (+ g (/ i y)))) g)",
        "(fn f ((y u8)) u8 (for i u8 1 5 (let k u8 (+ y i)) (set g (/ k y))) g)",
        "(fn f ((y u8)) u8 (for i u8 1 5 (set h (/ g y)) (set g i)) h)",
        "(fn bump () void (set g (+ g 3)))\n"
        "(fn f ((y u8)) u8 (for i u8 1 5 (bump) (set h (+ h (/ g y)))) h)",
        "(fn f ((y u8)) u8 (let p (ptr u8) (addr_of (at a 2)))\n"
        "    (for i u8 1 5 (store p i) (set h (+ h (/ (at a 2) y)))) h)",
        "(fn f ((y u8)) u8 (for i u8 1 5 (set h (+ h (/ scr y)))) h)",
        "(fn f ((y u8)) u8 (let p (ptr u8) (addr_of (at a 2)))\n"
        "    (for i u8 1 5 (set h (+ h (/ (load p) y)))) h)",
        "(fn rd ((v u8)) u8 (/ g v))\n"
        "(fn f ((y u8)) u8 (for i u8 1 5 (set h (+ h (rd y))) (set g (+ g i))) h)",
    };
    for (const char* text : kept) {
        Opted o = Optimize(With(text), NoCollapse());
        int division = LoopDepth(o, "t.f", ExprKind::Binary, kDiv);
        int call = LoopDepth(o, "t.f", ExprKind::Call);
        bool stayed = (division == 1 || call == 1) && division != 0 && call != 0;
        if (!stayed) {
            std::fprintf(stderr, "moved out of the loop:\n%s\n", text);
        }
        CHECK(results, o.ok && stayed);
        CheckProgram(results, With(text), "t.f", {3});
    }
    // effects that may change anything keep everything in the loop: jsr, io_write
    ExpectDepth(results,
                With("(fn f ((x u8) (y u8)) u8 (for i u8 1 5 (jsr 0xFBDD) (set h (+ h (/ x y))))"
                     " h)"),
                kDiv, 1);
    ExpectDepth(results,
                With("(fn f ((x u8) (y u8)) u8 (for i u8 1 5 (io_write u8 0xC030 i)"
                     " (set h (/ x y))) h)"),
                kDiv, 1);
    // in an arm that runs on one pass only, or in a loop inside that may not run: moving it
    // would cost cycles whenever the arm or the loop does not run
    ExpectDepth(results,
                With("(fn f ((x u8) (y u8)) u8\n"
                     "    (for i u8 0 8 (if (= i 5) (set g (/ x y)) (set h i))) g)"),
                kDiv, 1);
    ExpectDepth(results,
                With("(fn f ((x u8) (y u8)) u8\n"
                     "    (for i u8 0 8 (for j u8 0 i (set g (+ g (/ x y))))) g)"),
                kDiv, 1);
    // a range with an effect cannot be tested twice: the body's work stays
    ExpectDepth(results,
                With("(fn bump () u8 (set h (+ h 1)) h)\n"
                     "(fn f ((x u8) (y u8)) u8 (for i u8 0 (bump) (set g (+ g (/ x y)))) g)"),
                kDiv, 1);
    // cheap work stays where it is: reading a local would cost as much
    ExpectDepth(results, With("(fn f ((x u8)) u8 (for i u8 1 5 (set h (+ h (+ x 1)))) h)"), kAdd,
                1);
}

// a node kind the passes do not know keeps the loop's work in it
void UnknownNodes(TestResults& results) {
    std::string text = With("(fn f ((x u8) (y u8)) u8\n"
                            "    (for i u8 1 5 (set h 77) (set g (/ x y))) g)");
    Built built = BuildText(text);
    CHECK(results, built.ok);
    if (!built.ok) {
        return;
    }
    tisple::Program program = built.program;
    for (tisple::Expr& e : program.exprs) {
        if (e.kind == ExprKind::Set && e.b >= 0 &&
            program.exprs[static_cast<std::size_t>(e.b)].value == 77) {
            e.kind = static_cast<ExprKind>(200);
        }
    }
    tisple::OptReport report;
    tisple::OptimizeProgram(program, tisple::OptOptions(), report);
    Opted o;
    o.ok = true;
    o.program = program;
    CHECK(results, LoopDepth(o, "t.f", ExprKind::Binary, kDiv) == 1);
}

} // namespace

void RunOptLicmTests(TestResults& results) {
    Moves(results);
    Stays(results);
    UnknownNodes(results);
}

} // namespace tisple_tests
