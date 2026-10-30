// slice 6: inlining at the mid level (src/opt/inline.cpp) and divisions the ranges pass works
// out (ranges_divs.cpp): which calls become copies, how returns and arguments come out, what
// stays a call; each program also runs from main on the 6502 against the interpreter, which
// runs it as written

#include "test_backend.hpp"
#include "test_opt.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

using tisple::ExprKind;

// main on the 6502 against the interpreter, functions main does not call dropped (as fuzzing
// and the race build them)
void CheckMain(TestResults& results, const std::string& text) {
    Compiled c = CompileMainText(text);
    Diff d;
    bool ok = c.ok && StartDiff(d, c) && DiffCall(d, "t.main", {});
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s%s\n", text.c_str(), d.message.c_str(),
                     c.errors.c_str());
    }
    CHECK(results, ok);
}

// calls left in t.main after every pass
void ExpectCalls(TestResults& results, const std::string& text, int want,
                 const tisple::OptOptions& options = {}) {
    Opted o = OptimizeInlined(text, options);
    int got = CountKind(o, "t.main", ExprKind::Call);
    if (got != want) {
        std::fprintf(stderr, "%d calls left in t.main, not %d:\n%s\n", got, want, text.c_str());
    }
    CHECK(results, o.ok && got == want);
    CheckMain(results, text);
}

const char* const kGlobals = "(let g u8 7)\n(let h u8 200)\n(let w u16 1000)\n";

std::string With(const std::string& text) {
    return std::string(kGlobals) + text;
}

void Copied(TestResults& results) {
    const char* const copied[] = {
        // a return in the then arm: the rest is the else
        "(fn f ((x u8)) u8 (if (> x 9) (return (+ x 1))) (set g (+ g x)) (* x 3))\n"
        "(fn main () u8 (set h (f g)) (set h (+ h (f 12))) (+ h g))",
        // in the else arm, and in both
        "(fn f ((x u8)) u8 (if (< x 9) (set g 1) (return 4)) (set g (+ g x)) x)\n"
        "(fn k ((x u8)) u8 (if (= x 0) (return 1) (return 2)))\n"
        "(fn main () u8 (set h (f g)) (set g (+ (f 30) (k h))) (+ h g))",
        // a void function's returns
        "(fn f ((x u8)) void (if (= x 7) (return)) (set g (+ g 1)) (if (> g 9) (do (set h 0) "
        "(return))) (set h (+ h 1)))\n"
        "(fn main () u8 (f g) (+ h g))",
        // (set g (f ...)) of the global the callee writes: g set where each way ends
        "(fn f ((x u8)) u8 (set g (+ g 1)) (if (> x 100) (return g)) (+ g x))\n"
        "(fn main () u8 (set g (f g)) (set g (f h)) g)",
        // an argument that reads what the callee changes: the value before the call
        "(fn f ((x u8) (y u8)) u8 (set g 5) (+ x (* y g)))\n"
        "(fn main () u8 (set h (f g (+ g 1))) (set h (+ h (f (+ g g) g))) (+ h g))",
        // a parameter the callee sets
        "(fn f ((x u8)) u8 (set x (+ x 1)) (set g x) (* x 2))\n"
        "(fn main () u8 (let a u8 4) (set h (f a)) (+ h a))",
        // calls as each other's arguments, and in a let
        "(fn f ((x u8)) u8 (+ x g))\n(fn k ((x u8) (y u8)) u8 (set g (^ g y)) (- x y))\n"
        "(fn main () u8 (let a u8 (k (f 3) (f g))) (set h (+ a (k g (f a)))) (+ h g))",
        // in a loop, and in a while's condition
        "(fn f ((x u8)) u8 (+ x g))\n(fn main () u8 (for i u8 0 5 (set w (+ w (as u16 (f (+ i "
        "1))))))\n"
        "    (while (< (f h) 50) (set h (+ h 1))) (+ h (as u8 w)))",
        // a local of the callee whose let the passes took away (fuzz seed 10010645: its set went
        // to the callee's frame, which shares bytes with f's parameters)
        "(fn f ((a u8) (b u8)) u8 (let s u8 a) (for i u8 0 (& b 7) (set s (+ (* s 3) i)))\n"
        "    (if (> s a) (set h (+ h s))) (+ s (* a b)))\n"
        "(fn k () u8 (let v u8 255) (set v (+ g 1)) (set g (^ g v)) (set g (+ g v)) g)\n"
        "(fn main () u8 (set h (f g 3)) (set h (+ h (f (- g 154) (k)))) h)",
    };
    for (const char* text : copied) {
        std::string t = With(text);
        Opted o = OptimizeInlined(t);
        // f called from several places with a loop stays a call; the rest are copied
        int calls = CountKind(o, "t.main", ExprKind::Call);
        bool loop_f = std::string(text).find("(for i u8 0 (& b 7)") != std::string::npos;
        if (calls != (loop_f ? 2 : 0)) {
            std::fprintf(stderr, "%d calls left in t.main:\n%s\n", calls, t.c_str());
        }
        CHECK(results, o.ok && calls == (loop_f ? 2 : 0));
        CheckMain(results, t);
    }
}

void Kept(TestResults& results) {
    // a return inside a loop: a call
    ExpectCalls(results,
                With("(fn f ((x u8)) u8 (for i u8 0 4 (if (= i x) (return i))) (set g 1) 9)\n"
                     "(fn main () u8 (set h (f g)) (+ h (f 2)))"),
                2);
    // an arm that may or may not return: a call
    ExpectCalls(results,
                With("(fn f ((x u8)) u8 (if (> x 3) (do (if (= g 0) (return 1)) (set g 0)))\n"
                     "    (set h 2) x)\n(fn main () u8 (+ (f g) (f h)))"),
                2);
    // what --no-inline names
    tisple::OptOptions never;
    never.no_inline = {"t.f"};
    ExpectCalls(results, With("(fn f ((x u8)) u8 (set g (+ g x)) g)\n(fn main () u8 (f h))"), 1,
                never);
    // a function whose address is taken
    ExpectCalls(results,
                With("(fn f ((x u8)) u8 (set g (+ g x)) g)\n"
                     "(fn main () u8 (let fp (fn (u8) u8) f) (+ (f h) (fp 3)))"),
                1);
    // the handler and what it calls stay apart
    std::string irq = "(let ticks u8 0)\n(fn bump ((x u8)) void (set ticks (+ ticks x)))\n"
                      "(fn tick () void :interrupt (bump 1))\n(fn main () u8 (cli) ticks)";
    Opted o = OptimizeInlined(irq);
    CHECK(results, o.ok && CountKind(o, "t.tick", ExprKind::Call) == 1);
}

void Emptied(TestResults& results) {
    // f's only call is copied: its body goes, with keep_bodies off
    std::string text = With("(fn f ((x u8)) u8 (set g (+ g x)) (for i u8 0 (& x 3) "
                            "(set h (+ h i))) g)\n(fn main () u8 (f h))");
    Opted o = OptimizeInlined(text);
    CHECK(results, o.ok && CountKind(o, "t.f", ExprKind::Set) == 0 &&
                       CountKind(o, "t.main", ExprKind::Call) == 0);
    tisple::OptOptions keep;
    keep.keep_bodies = true;
    Opted kept = OptimizeInlined(text, keep);
    CHECK(results, kept.ok && CountKind(kept, "t.f", ExprKind::Set) > 0);
    CheckMain(results, text);
}

// x takes ten values of its type in turn, from a table no store reaches (no pass knows which)
const char* Starts(const std::string& type) {
    if (type == "u8") {
        return "0 1 2 3 127 128 200 254 255 6";
    }
    if (type == "i8") {
        return "0 1 2 -1 -2 127 -128 5 -7 64";
    }
    if (type == "u16") {
        return "0 1 2 255 256 32767 32768 65534 65535 1000";
    }
    return "0 1 -1 2 -2 32767 -32768 300 -300 7";
}

void Divisions(TestResults& results) {
    struct Case {
        const char* type;
        const char* value; // in terms of x
        bool gone;         // no division left
    };
    const Case cases[] = {
        {"u8", "(/ x 1)", true},
        {"u8", "(% x 1)", true},
        {"i16", "(/ x 1)", true},
        {"i8", "(% x 1)", true},
        {"u8", "(% x (- g g))", true},
        {"i16", "(% x (- x x))", true},
        {"u16", "(/ x (- x x))", true},
        {"i8", "(/ x (- x x))", false},
        {"u8", "(/ x (| x 1))", true},
        {"u16", "(% x (| x 1))", true},
        {"i8", "(/ x (| x 1))", false},
        {"u8", "(/ (% x (| g 1)) (| g 1))", true},
        {"u16", "(/ x x)", true},
        {"i16", "(/ x x)", false},
        {"u8", "(/ (& x 7) (| (& g 15) 8))", true},
        {"u8", "(% (& x 7) (| (& g 15) 8))", true},
    };
    for (const Case& c : cases) {
        std::string t = c.type;
        std::string text = std::string(kGlobals) + "(let tv (array " + t + " 10) (make (array " +
                           t + " 10) " + Starts(t) + "))\n(let acc u16 0)\n" +
                           "(fn main () u16 (for i u8 0 10 (let x " + t + " (at tv i))\n" +
                           "    (set acc (+ (* acc 3) (as u16 " + c.value + ")))) acc)";
        Opted o = OptimizeInlined(text);
        int divs = 0;
        for (tisple::Op op : {tisple::Op::Div, tisple::Op::Mod}) {
            divs += LoopDepth(o, "t.main", ExprKind::Binary, static_cast<int>(op)) >= 0 ? 1 : 0;
        }
        if (c.gone && divs != 0) {
            std::fprintf(stderr, "a division left:\n%s\n", text.c_str());
        }
        CHECK(results, o.ok && (!c.gone || divs == 0));
        CheckMain(results, text);
    }
}

// collapse (collapse_scan.cpp, slice 6): a field of an element whose index is not known is
// that field of every element, apart from the struct's other fields (seed 62)
void Fields(TestResults& results) {
    const std::string structs = "(struct S ((a u8) (b i16) (c bool)))\n(let sa (array S 8))\n"
                                "(let k u8 3)\n(let out u8 0)\n";
    struct Case {
        const char* body;
        bool collapses;
    };
    const Case cases[] = {
        // writes b of element 1, reads a of any: every pass the same
        {"(for i u8 0 20 (set (at sa 1 b) -7) (set out (+ (at sa (& k 7) a) 1)))", true},
        // writes a of element 1, reads a of any: may read what it wrote
        {"(for i u8 0 20 (set (at sa 1 a) (+ out 1)) (set out (+ (at sa (& k 7) a) 1)))", false},
        // an index that may leave the array: the whole variable
        {"(for i u8 0 20 (set (at sa 1 b) -7) (set out (+ (at sa (& k 15) a) 1)))", false},
    };
    for (const Case& c : cases) {
        std::string text = structs +
                           "(fn main () u8 (set k (io_read u8 0xC000)) (set (at sa 3 a) 9) " +
                           c.body + " out)";
        Opted o = OptimizeInlined(text);
        bool loop = CountKind(o, "t.main", ExprKind::For) > 0;
        if (loop == c.collapses) {
            std::fprintf(stderr, "the loop %s:\n%s\n", loop ? "stays" : "went", text.c_str());
        }
        CHECK(results, o.ok && loop != c.collapses);
        CheckMain(results, text);
    }
}

// liveness (live.cpp, slice 6): a for with a literal range makes its first pass, so what that
// pass sets before reading it is dead before the loop (seed 1278); a break that may come
// first keeps it
void FirstPass(TestResults& results) {
    std::string dead = With("(fn main () u8 (set g 5) (for i u8 0 20 (set g (+ i h)) (set (at "
                            "wb i) g)) g)\n(let wb (array u8 32))");
    CheckMain(results, dead);
    for (const char* h : {"0", "7", "200"}) {
        std::string kept =
            std::string("(let g u8 7)\n(let h u8 ") + h +
            ")\n(let wb (array u8 32))\n"
            "(fn main () u8 (set h (io_read u8 0xC000)) (set g 5)\n"
            "    (for i u8 0 20 (if (= i h) (break)) (set g i) (set (at wb i) g)) g)";
        CheckMain(results, kept);
    }
}

// pointers to known elements (pointers.cpp): loads and stores at literal counts from them are
// the elements' reads and sets, also when the pointer goes elsewhere too
void KnownElements(TestResults& results) {
    const std::string arrays =
        "(let wb (array u8 1024))\n(let pt (array u8 16))\n(let o u8 0)\n"
        "(fn poke ((p (ptr u8)) (v u8)) void (store p v) (store (+ p 1) v))\n";
    const char* const bodies[] = {
        "(let q (ptr u8) (addr_of (at wb 3))) (store (+ q 254) 5) (store q (+ (load q) 1))\n"
        "    (set o (+ (load (+ q 254)) (at wb 3)))",
        "(let q (ptr u8) (addr_of (at pt 2))) (let r (ptr u8) q) (store (- r 1) 7)\n"
        "    (poke q 9) (set o (+ (load (+ r 1)) (at pt 1)))",
        // past the end: through the pointer as it was (the next variable's bytes)
        "(let q (ptr u8) (addr_of (at pt 15))) (set o (+ (load q) (load (- q 15))))",
    };
    for (const char* body : bodies) {
        std::string text = arrays + "(fn main () u8 " + body + " o)";
        CheckMain(results, text);
    }
    Opted o = OptimizeInlined(arrays + "(fn main () u8 " + bodies[0] + " o)");
    CHECK(results, o.ok && CountKind(o, "t.main", ExprKind::Load) == 0 &&
                       CountKind(o, "t.main", ExprKind::Store) == 0);
}

} // namespace

void RunOptInlineTests(TestResults& results) {
    KnownElements(results);
    FirstPass(results);
    Fields(results);
    Copied(results);
    Kept(results);
    Emptied(results);
    Divisions(results);
}

} // namespace tisple_tests
