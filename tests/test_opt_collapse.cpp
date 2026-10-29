// loop collapse (src/opt/collapse.cpp) and unread arguments (src/opt/args.cpp): loops whose
// passes all do the same, or whose last pass decides, run once; accumulators add up at once;
// what must stay a loop; each program also run on the 6502 against the interpreter, which runs
// it as written

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

int Loops(const Opted& o) {
    return CountKind(o, "t.f", ExprKind::For) + CountKind(o, "t.f", ExprKind::While);
}

// no loop left in t.f, and the 6502 agrees with the interpreter on each argument
void ExpectCollapsed(TestResults& results, const char* text,
                     const std::vector<std::uint16_t>& args) {
    Opted o = Optimize(With(text));
    if (Loops(o) != 0) {
        std::fprintf(stderr, "a loop stayed:\n%s\n", text);
    }
    CHECK(results, o.ok && Loops(o) == 0);
    for (std::uint16_t arg : args) {
        CheckProgram(results, With(text), "t.f", {arg});
    }
}

void ExpectKept(TestResults& results, const char* text, int loops) {
    Opted o = Optimize(With(text));
    if (Loops(o) != loops) {
        std::fprintf(stderr, "%d loops, not %d:\n%s\n", Loops(o), loops, text);
    }
    CHECK(results, o.ok && Loops(o) == loops);
    // the interpreter stops at machine code and the race's I/O: only the count is checked
    if (std::string(text).find("io_write") == std::string::npos) {
        CheckProgram(results, With(text), "t.f", {3});
    }
}

void Same(TestResults& results) {
    const char* collapsed[] = {
        // a body that reads only what no pass writes
        "(fn f ((x u8)) u8 (for i u8 0 10 (set g (+ x 1)) (set (at a 2) (* x 3))) g)",
        // a counting while: the counter ends where it would
        "(fn f ((x u8)) u8 (let c u8 0) (while (< c 5) (set c (+ c 1)) (set (at a 2) x)) c)",
        // nested: the inner one first, then the outer one
        "(fn f ((x u8)) u8 (for i u8 0 3 (for j u8 0 4 (set (at a 1) x))) (at a 1))",
        // its own lets and loops are set afresh on every pass
        "(fn f ((x u8)) u8 (for i u8 0 6 (let t u8 (+ x 2)) (let s u8 0)\n"
        "    (for k u8 0 3 (set s (+ s t))) (set h s)) h)",
        // different elements and fields of one array: one read, another written
        "(fn f ((x u8)) u8 (for i u8 0 9 (set (at a 1) (at a 2))) (at a 1))",
        // a store through a pointer into one array, reading another
        "(fn f ((x u8)) u8 (let p (ptr u8) (addr_of (at a 3)))\n"
        "    (for i u8 0 4 (store p (+ (at b 1) x))) (at a 3))",
        // through a parameter, followed to what the calls pass
        "(fn put ((p (ptr u8)) (v u8)) void (store p v))\n"
        "(fn f ((x u8)) u8 (for i u8 0 4 (put (addr_of (at a 5)) (at b 0))) (at a 5))",
        // a call that writes what nobody in the loop reads
        "(fn put ((v u8)) void (set h v))\n"
        "(fn f ((x u8)) u8 (for i u8 0 4 (put x)) h)",
    };
    for (const char* text : collapsed) {
        ExpectCollapsed(results, text, {3, 200});
    }
}

void Accumulators(TestResults& results) {
    const char* collapsed[] = {
        "(fn f ((x u8)) u16 (for i u8 0 7 (set gw (+ gw 300))) gw)",
        "(fn f ((x u8)) u16 (for i u8 0 7 (set gw (+ gw (as u16 x)))) gw)",
        "(fn f ((x u8)) u8 (for i u8 0 5 (set g (- g 3))) g)",
        "(fn f ((x u8)) u8 (for i u8 0 5 (set g (^ g x))) g)",
        "(fn f ((x u8)) u8 (for i u8 0 4 (set g (^ g x))) g)",
        // | and & add nothing after the first pass, whatever the count
        "(fn f ((x u8)) u8 (for i u8 x 9 (set g (| g 6))) g)",
        // a step read from the body's own let, which is the same every pass
        "(fn f ((x u8)) u8 (for i u8 0 3 (let t u8 (+ x 1)) (set g (+ g t))) g)",
        // in a do of the body
        "(fn f ((x u8)) u16 (let c u8 0)\n"
        "    (while (< c 3) (set c (+ c 1)) (do (let n u16 (as u16 x)) (set gw (+ gw n)))) gw)",
        // a counting while's counter wrapping where the loop ends
        "(fn f ((x u8)) u8 (let c i8 120) (while (< c 127) (set c (+ c 1)) (set g (+ g 2)))\n"
        "    (as u8 c))",
    };
    for (const char* text : collapsed) {
        ExpectCollapsed(results, text, {3, 200});
    }
}

void LastPass(TestResults& results) {
    const char* collapsed[] = {
        "(fn f ((x u8)) u8 (for i u8 0 5 (set g (* i 3))) g)",
        "(fn f ((x u8)) u8 (let c u8 0) (while (< c 4) (set c (+ c 1)) (set h c)) (+ h c))",
        "(fn f ((x u8)) u8 (for i i8 -3 2 (set (at a 1) (as u8 i)) (set g (+ g 1))) (at a 1))",
        // a branch on what no pass changes, a write there
        "(fn f ((x u8)) u8 (for i u8 0 5 (if (> x 9) (set g i) (set h i))) (+ g h))",
        // a store through a pointer at the same place every pass
        ("(fn f ((x u8)) u8 (let p (ptr u8) (addr_of (at a 0))) (for i u8 0 4 (store p i))\n"
         "    (at a 0))"),
    };
    for (const char* text : collapsed) {
        ExpectCollapsed(results, text, {3, 200});
    }
    // a for inside whose range is the same on every pass makes the same passes: the loop around
    // it runs its last pass, the inner one stays
    std::string inner = With("(fn f ((x u8)) u8\n"
                             "    (for i u8 0 4 (for j u8 0 3 (set (at a j) (+ i j)))) (at a 2))");
    CHECK(results, Loops(Optimize(inner)) == 1);
    CheckProgram(results, inner, "t.f", {3});
    // one whose range the counter changes, or that may end early: both stay
    const char* ranged[] = {
        "(fn f ((x u8)) u8 (for i u8 0 4 (for j u8 0 i (set (at a j) i))) (at a 2))",
        ("(fn f ((x u8)) u8 (for i u8 0 4 (for j u8 0 3 (if (= i j) (break)) (set (at a j) i)))\n"
         "    (at a 2))"),
    };
    for (const char* text : ranged) {
        CHECK(results, Loops(Optimize(With(text))) == 2);
        CheckProgram(results, With(text), "t.f", {3});
    }
}

void Kept(TestResults& results) {
    const char* kept[] = {
        // a pass reads what an earlier one wrote
        "(fn f ((x u8)) u8 (for i u8 0 4 (set g (+ g h)) (set h g)) g)",
        "(fn f ((x u8)) u8 (let p (ptr u8) (addr_of (at a 3)))\n"
        "    (for i u8 0 4 (store p (+ (at a 3) 1))) (at a 3))",
        "(fn f ((x u8)) u8 (for i u8 0 4 (set (at a 1) (+ (at a 1) x))) (at a 1))",
        // an accumulator read elsewhere in the loop
        "(fn f ((x u8)) u8 (for i u8 0 4 (set g (+ g 1)) (set h g)) h)",
        // the counter decides whether, or where, a pass writes
        "(fn f ((x u8)) u8 (for i u8 0 4 (if (= i x) (set g 9))) g)",
        "(fn f ((x u8)) u8 (for i u8 0 4 (set (at a i) x)) (at a 2))",
        "(fn f ((x u8)) u8 (for i u8 0 4 (set g (+ g i))) g)",
        "(fn w ((v u8)) void (if (= v 2) (set g 5)))\n"
        "(fn f ((x u8)) u8 (for i u8 0 4 (w i)) g)",
        "(fn f ((x u8)) u8 (let p (ptr u8) (addr_of (at a 0))) (for i u8 0 4 (store (+ p i) x))\n"
        "    (at a 2))",
        // a pass that may end early
        "(fn f ((x u8)) u8 (for i u8 0 4 (set g x) (if (= x 3) (break))) g)",
        "(fn f ((x u8)) u8 (for i u8 0 4 (set g x) (if (= x 3) (return 7))) g)",
        // a while that is not a counting one
        "(fn f ((x u8)) u8 (let c u8 x) (while (< c 9) (set c (+ c 2)) (set h 1)) h)",
        // its counter read before its step and changed by a continue
        "(fn f ((x u8)) u8 (let c u8 0) (while (< c 4) (if (= x 99) (continue)) (set c (+ c 1))\n"
        "    (set h 1)) h)",
    };
    for (const char* text : kept) {
        ExpectKept(results, text, 1);
    }
    // what hardware may answer or notice: memory no variable uses, :address lets, I/O, waits
    const char* hardware[] = {
        "(fn f ((x u8)) u8 (for i u8 0 4 (set g (load (as (ptr u8) 0x4000)))) g)",
        "(fn f ((x u8)) u8 (for i u8 0 4 (store (as (ptr u8) 0x4000) x)) g)",
        "(fn f ((x u8)) u8 (for i u8 0 4 (set g scr)) g)",
        "(fn f ((x u8)) u8 (for i u8 0 4 (set scr x)) g)",
        "(fn f ((x u8)) u8 (for i u8 0 4 (io_write u8 0xC030 x)) g)",
        "(fn f ((x u8)) u8 (for i u8 0 4 (wait_cycles 100) (set g x)) g)",
    };
    for (const char* text : hardware) {
        Opted o = Optimize(With(text));
        CHECK(results, o.ok && Loops(o) == 1);
    }
    // a loop whose bounds may make it run no pass is behind a test of them
    std::string guarded = With("(fn f ((x u8)) u8 (for i u8 x 10 (set g 7)) g)");
    ExpectCount(results, guarded, "t.f", ExprKind::If, 1);
    CheckProgram(results, guarded, "t.f", {3});
    CheckProgram(results, guarded, "t.f", {12});
    // unless the shapes of its bounds say it runs
    ExpectCount(results, With("(fn f ((x u8)) u8 (for i i8 -4 (as i8 (& x 7)) (set g 1)) g)"),
                "t.f", ExprKind::If, 0);
}

void UnreadArgs(TestResults& results) {
    // the division for a parameter nobody reads is not worked out
    std::string unread = With("(fn two ((u u8) (v u8)) u8 (set u 4) v)\n"
                              "(fn f ((x u8)) u8 (two (/ x 3) x))");
    tisple::OptOptions no_args;
    no_args.disabled = {"args"};
    int division = 0;
    Opted o = Optimize(unread);
    for (std::size_t id = 0; id < o.program.exprs.size(); id++) {
        const tisple::Expr& e = o.program.exprs[id];
        division += e.kind == ExprKind::Binary && e.op == tisple::Op::Div ? 1 : 0;
    }
    CHECK(results, o.ok && division == 0);
    CheckProgram(results, unread, "t.f", {200});
    // read, or an argument with an effect or reading hardware: worked out
    const char* kept[] = {
        "(fn two ((u u8) (v u8)) u8 (+ u v))\n(fn f ((x u8)) u8 (two (/ x 3) x))",
        ("(fn bump () u8 (set g (+ g 1)) g)\n"
         "(fn two ((u u8) (v u8)) u8 v)\n(fn f ((x u8)) u8 (two (bump) x))"),
        "(fn two ((u u8) (v u8)) u8 v)\n(fn f ((x u8)) u8 (two scr x))",
    };
    for (const char* text : kept) {
        Opted with = Optimize(With(text));
        Opted without = Optimize(With(text), no_args);
        CHECK(results, with.ok && with.report.applied == without.report.applied);
        CheckProgram(results, With(text), "t.f", {200});
    }
}

} // namespace

void RunOptCollapseTests(TestResults& results) {
    Same(results);
    Accumulators(results);
    LastPass(results);
    Kept(results);
    UnreadArgs(results);
}

} // namespace tisple_tests
