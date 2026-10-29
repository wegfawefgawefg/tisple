// value ranges (src/opt/ranges.cpp): compares and match clauses the values of their operands
// decide, from shapes, loop counters and const tables; what they must not decide; each program
// also run on the 6502 against the interpreter, which runs it as written

#include "test_backend.hpp"
#include "test_opt.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

using tisple::ExprKind;

const char* const kGlobals = "(let g u8 0)\n(let h u8 0)\n(let gi i16 0)\n"
                             "(let a (array u8 8))\n(let scr u8 :address 0x0400)\n"
                             "(const KT (array u16 4) (make (array u16 4) 7 2 300 65535))\n";

std::string With(const std::string& text) {
    return std::string(kGlobals) + text;
}

// t.f keeps want ifs and matches (decided branches are cut by constant propagation)
void ExpectBranches(TestResults& results, const char* text, int want) {
    Opted o = Optimize(With(text));
    int got = CountKind(o, "t.f", ExprKind::If) + CountKind(o, "t.f", ExprKind::Match);
    if (got != want) {
        std::fprintf(stderr, "%d branches, not %d:\n%s\n", got, want, text);
    }
    CHECK(results, o.ok && got == want);
    CheckProgram(results, With(text), "t.f", {3});
    CheckProgram(results, With(text), "t.f", {250});
}

void Decided(TestResults& results) {
    const char* decided[] = {
        // shapes: a mask, a remainder, a quotient, a widened byte, a shift
        "(fn f ((x u8)) u8 (if (< (& x 7) 8) (set g 1)) g)",
        "(fn f ((x u8)) u8 (if (> (% x 10) 9) (set g 1)) g)",
        "(fn f ((x u8)) u8 (if (<= (/ x 3) 85) (set g 1)) g)",
        "(fn f ((x u8)) u8 (if (>= (as i16 x) 0) (set g 1)) g)",
        "(fn f ((x u8)) u8 (if (= (>> x 5) 8) (set g 1)) g)",
        "(fn f ((x u8)) u8 (if (!= (+ (& x 3) (& x 3)) 7) (set g 1)) g)",
        // the type's own range
        "(fn f ((x u8)) u8 (if (> -32768 gi) (set g 1)) g)",
        "(fn f ((x u8)) u8 (if (<= x 255) (set g 1)) g)",
        // a const table's values, whatever the index
        "(fn f ((x u8)) u8 (if (>= (at KT (& x 3)) 2) (set g 1)) g)",
        // a for's counter, and a counting while's around its step
        "(fn f ((x u8)) u8 (for i u8 0 5 (if (> i 10) (set g i) (set (at a i) x))) g)",
        ("(fn f ((x u8)) u8 (let c u8 0) (while (< c 5) (set c (+ c 1))\n"
         "    (match c (15 (set g x)) (28 (set h x)) (_ (set (at a c) 1)))) g)"),
        // an and or an or decided by its right side
        "(fn f ((x u8)) u8 (if (and (> x 4) (< (& x 7) 0)) (set g 1)) g)",
        "(fn f ((x u8)) u8 (if (or (> x 4) (<= (& x 7) 7)) (set g 1)) g)",
    };
    for (const char* text : decided) {
        ExpectBranches(results, text, 0);
    }
}

void Undecided(TestResults& results) {
    const char* kept[] = {
        "(fn f ((x u8)) u8 (if (< (& x 7) 7) (set g 1)) g)",
        // a sum that may wrap
        "(fn f ((x u8)) u8 (if (> (+ x 1) 0) (set g 1)) g)",
        // hardware may answer anything
        "(fn f ((x u8)) u8 (if (< (& scr 7) 8) (set g 1)) g)",
        // a clause the counter reaches
        ("(fn f ((x u8)) u8 (let c u8 0) (while (< c 5) (set c (+ c 1))\n"
         "    (match c (3 (set g x)) (_ (set (at a c) 1)))) g)"),
        // a counter written twice in its loop
        ("(fn f ((x u8)) u8 (let c u8 0) (while (< c 5) (set c (+ c 1)) (if (> c 9) (set g 1))\n"
         "    (set c (+ c 1))) g)"),
        // an and whose left side has an effect stays an and
        ("(fn bump () u8 (set h (+ h 1)) h)\n"
         "(fn f ((x u8)) u8 (if (and (> (bump) 4) (< (& x 7) 0)) (set g 1)) g)"),
    };
    for (const char* text : kept) {
        Opted o = Optimize(With(text));
        int got = CountKind(o, "t.f", ExprKind::If) + CountKind(o, "t.f", ExprKind::Match);
        if (got == 0) {
            std::fprintf(stderr, "a branch went:\n%s\n", text);
        }
        CHECK(results, o.ok && got >= 1);
        CheckProgram(results, With(text), "t.f", {3});
        CheckProgram(results, With(text), "t.f", {250});
    }
}

} // namespace

void RunOptRangesTests(TestResults& results) {
    Decided(results);
    Undecided(results);
}

} // namespace tisple_tests
