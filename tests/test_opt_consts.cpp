// constant propagation and folding across statements (src/opt/consts.cpp): what becomes a
// literal, which branches are cut, and what must stay unknown; each program also run on the
// 6502 against the interpreter, which runs it as written

#include "test_backend.hpp"
#include "test_opt.hpp"

#include <string>

namespace tisple_tests {

namespace {

using tisple::ExprKind;

const char* const kGlobals = "(let g u8 0)\n(let h u8 0)\n(let gw u16 0)\n(let ge i8 0)\n"
                             "(let a (array u8 8))\n(let scr u8 :address 0x0400)\n"
                             "(const KT (array u16 4) (make (array u16 4) 4406 2 23778 65535))\n";

std::string With(const std::string& text) {
    return std::string(kGlobals) + text;
}

void Locals(TestResults& results) {
    // a local's known value goes into its reads, the arithmetic folds, the lets go
    std::string folded = With("(fn f ((x u8)) u8 (let k u8 5) (let m u8 (* k 3)) (+ x m))");
    ExpectCount(results, folded, "t.f", ExprKind::Let, 0);
    ExpectCount(results, folded, "t.f", ExprKind::Binary, 1);
    CheckProgram(results, folded, "t.f", {9});
    // a const array at a known index, a wide value through a narrowing
    std::string table = With("(fn f ((x u8)) u16 (let i u8 2) (let w u16 (at KT i))\n"
                             "    (+ (as u16 x) (+ w (as u16 (as u8 w)))))");
    ExpectCount(results, table, "t.f", ExprKind::Index, 0);
    CheckProgram(results, table, "t.f", {200});
    // division by zero folds to the defined result: all ones, 1 for a negative signed
    // dividend, the dividend for a remainder
    std::string zero = With("(fn f () u8 (let z u8 0) (let n u8 7) (let s i8 -7) (let t i8 0)\n"
                            "    (set ge (/ s t)) (set h (% n z)) (/ n z))");
    ExpectCount(results, zero, "t.f", ExprKind::Binary, 0);
    CheckProgram(results, zero, "t.f", {});
    // a parameter is unknown, and a value from a call
    std::string unknown = With("(fn bump () u8 (set g (+ g 1)) g)\n"
                               "(fn f ((x u8)) u8 (let k u8 (bump)) (+ (+ k x) 1))");
    ExpectCount(results, unknown, "t.f", ExprKind::Binary, 2);
}

void Branches(TestResults& results) {
    // an if on a known condition becomes its arm; what the other arm writes does not count
    std::string cut = With("(fn f ((x u8)) u8 (let k u8 1)\n"
                           "    (if (= k 1) (set k 2) (set k 5)) (+ k x))");
    ExpectCount(results, cut, "t.f", ExprKind::If, 0);
    ExpectCount(results, cut, "t.f", ExprKind::Binary, 1);
    CheckProgram(results, cut, "t.f", {3});
    // a match on a known value becomes its clause, or nothing
    std::string match = With("(fn f ((x u8)) u8 (let e u8 2)\n"
                             "    (match e (1 (set g 1)) (2 (set g 2)) (_ (set g 3)))\n"
                             "    (match (+ e 5) (1 (set h 1)) (3 (set h 2)) (_ (do))) (+ g x))");
    ExpectCount(results, match, "t.f", ExprKind::Match, 0);
    ExpectCount(results, match, "t.f", ExprKind::Set, 1);
    CheckProgram(results, match, "t.f", {3});
    // a while whose condition is false the first time
    std::string never = With("(fn f () u8 (let c u8 10) (while (< c 5) (set g (+ g 1))\n"
                             "    (set c (+ c 1))) g)");
    ExpectCount(results, never, "t.f", ExprKind::While, 0);
    CheckProgram(results, never, "t.f", {});
    // and/or on a known left side
    std::string logic = With("(fn f ((x u8)) u8 (let t bool true) (let n bool false)\n"
                             "    (if (and t (> x 3)) (set g 1)) (if (or n (< x 2)) (set h 1))\n"
                             "    (if (and n (> x 9)) (set g 7)) (+ g h))");
    ExpectCount(results, logic, "t.f", ExprKind::And, 0);
    ExpectCount(results, logic, "t.f", ExprKind::Or, 0);
    ExpectCount(results, logic, "t.f", ExprKind::If, 2);
    CheckProgram(results, logic, "t.f", {5});
    CheckProgram(results, logic, "t.f", {1});
    // an unknown condition keeps both arms and what they write joins
    std::string both = With("(fn f ((x u8)) u8 (let k u8 1) (if (> x 4) (set k 2)) (+ k x))");
    ExpectCount(results, both, "t.f", ExprKind::If, 1);
    CheckProgram(results, both, "t.f", {9});
    // the same value on both sides still folds, and the if is left with nothing to do
    std::string same = With("(fn f ((x u8)) u8 (let k u8 1) (if (> x 4) (set k 3) (set k 3))\n"
                            "    (+ k 1))");
    ExpectCount(results, same, "t.f", ExprKind::Binary, 0);
    ExpectCount(results, same, "t.f", ExprKind::If, 0);
    CheckProgram(results, same, "t.f", {9});
}

void Loops(TestResults& results) {
    // a value set on every pass is not known after it, nor inside
    std::string carried = With("(fn f () u8 (let s u8 0) (for i u8 0 4 (set s (+ s 1))) s)");
    ExpectCount(results, carried, "t.f", ExprKind::Binary, 1);
    CheckProgram(results, carried, "t.f", {});
    // one the loop never changes is known inside it
    std::string held = With("(fn f () u8 (let k u8 3) (for i u8 0 4 (set g (+ g k))) g)");
    ExpectCount(results, held, "t.f", ExprKind::Let, 0);
    CheckProgram(results, held, "t.f", {});
    // changed on some passes only
    std::string some = With("(fn f () u8 (let k u8 3) (while (< g 10) (if (= g 6) (set k 1))\n"
                            "    (set g (+ g k))) (+ g k))");
    ExpectCount(results, some, "t.f", ExprKind::Let, 1);
    CheckProgram(results, some, "t.f", {});
    // a while true left only by a break: after it, what the break saw
    std::string exits =
        With("(fn f ((x u8)) u8 (let k u8 1)\n"
             "    (while true (if (> g 3) (do (set k 2) (break))) (set g (+ g 1)))\n"
             "    (+ k x))");
    ExpectCount(results, exits, "t.f", ExprKind::Let, 0);
    CheckProgram(results, exits, "t.f", {4});
    // a continue carries its value to the next pass
    std::string next = With("(fn f () u8 (let k u8 1) (let c u8 0)\n"
                            "    (while (< c 6) (set c (+ c 1)) (set g (+ g k))\n"
                            "        (if (= c 2) (do (set k 4) (continue))) (set k 1)) g)");
    ExpectCount(results, next, "t.f", ExprKind::Let, 2);
    CheckProgram(results, next, "t.f", {});
}

void Globals(TestResults& results) {
    // a global between its store and a read nothing can change
    std::string plain = With("(fn rd () u8 h)\n"
                             "(fn f () u8 (set g 7) (let r u8 (rd)) (set h (+ g 1)) (+ r g))");
    ExpectCount(results, plain, "t.f", ExprKind::Binary, 1);
    CheckProgram(results, plain, "t.f", {});
    // must stay unknown: a call that writes it, jsr, io_write, an :address let, a variable
    // whose address is taken (a store through its pointer), a node the passes do not know
    const char* kept[] = {
        "(fn w () void (set g 9))\n(fn f () u8 (set g 7) (w) (+ g 1))",
        "(fn f () u8 (set g 7) (jsr 0xFBDD) (+ g 1))",
        "(fn f () u8 (set g 7) (io_write u8 0xC030 0) (+ g 1))",
        "(fn f () u8 (set scr 5) (+ scr 1))",
        "(fn f () u8 (let d u8 5) (let p (ptr u8) (addr_of d)) (store p 9) (+ d 1))",
    };
    for (const char* text : kept) {
        ExpectCount(results, With(text), "t.f", ExprKind::Binary, 1);
    }
    CheckProgram(results, With(kept[0]), "t.f", {});
    CheckProgram(results, With(kept[4]), "t.f", {});
    // a global is unknown when a function starts: a caller may have set it
    ExpectCount(results, With("(fn f () u8 (+ g 1))\n(fn main () void (set g 4) (set h (f)))"),
                "t.f", ExprKind::Binary, 1);
}

// a node kind the passes do not know makes every variable unknown after it
void UnknownNodes(TestResults& results) {
    std::string text = With("(fn f () u8 (let k u8 3) (set h 77) (+ k 1))");
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
    CHECK(results, CountKind(o, "t.f", ExprKind::Binary) == 1);
}

} // namespace

void RunOptConstsTests(TestResults& results) {
    Locals(results);
    Branches(results);
    Loops(results);
    Globals(results);
    UnknownNodes(results);
}

} // namespace tisple_tests
