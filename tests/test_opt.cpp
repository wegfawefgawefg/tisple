// the mid-level passes (src/opt): what each removes, what it must keep, and the optimised
// code on the 6502 against the interpreter, which runs the program as written

#include "test_opt.hpp"

#include "backend/analysis.hpp"
#include "test_backend.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace tisple_tests {

Opted Optimize(const std::string& text, const tisple::OptOptions& options) {
    Opted out;
    Built built = BuildText(text);
    out.ok = built.ok;
    if (!built.ok) {
        std::fprintf(stderr, "program does not check:\n%s\n%s\n", text.c_str(),
                     ErrorText(built).c_str());
        return out;
    }
    out.program = built.program;
    tisple::FoldConstants(out.program);
    tisple::OptimizeProgram(out.program, options, out.report);
    return out;
}

namespace {

int CountIn(const tisple::Program& program, tisple::ExprId id, tisple::ExprKind kind) {
    if (id < 0) {
        return 0;
    }
    const tisple::Expr& e = program.exprs[static_cast<std::size_t>(id)];
    int count = e.kind == kind ? 1 : 0;
    count +=
        CountIn(program, e.a, kind) + CountIn(program, e.b, kind) + CountIn(program, e.c, kind);
    bool list = e.kind == tisple::ExprKind::Call || e.kind == tisple::ExprKind::StructLit ||
                e.kind == tisple::ExprKind::Do;
    for (std::uint32_t i = 0; list && i < e.count; i++) {
        count += CountIn(program, program.lists[e.first + i], kind);
    }
    for (std::uint32_t i = 0; e.kind == tisple::ExprKind::Match && i < e.count; i++) {
        count += CountIn(program, program.clauses[e.first + i].body, kind);
    }
    return count;
}

} // namespace

int CountKind(const Opted& o, const std::string& function, tisple::ExprKind kind) {
    std::int32_t f = tisple::FindFunction(o.program, function);
    if (f < 0) {
        return -1;
    }
    return CountIn(o.program, o.program.functions[static_cast<std::size_t>(f)].body, kind);
}

void ExpectCount(TestResults& results, const std::string& text, const std::string& function,
                 tisple::ExprKind kind, int want) {
    Opted o = Optimize(text);
    int got = CountKind(o, function, kind);
    if (got != want) {
        std::fprintf(stderr, "after the passes, %s has %d nodes of kind %d, not %d:\n%s\n",
                     function.c_str(), got, static_cast<int>(kind), want, text.c_str());
    }
    CHECK(results, o.ok && got == want);
}

namespace {

using tisple::ExprKind;

const char* const kGlobals = "(let g u8 0)\n(let h u8 0)\n(let gw u16 0)\n"
                             "(let a (array u8 8))\n(let scr u8 :address 0x0400)\n";

std::string With(const std::string& text) {
    return std::string(kGlobals) + text;
}

void DeadLets(TestResults& results) {
    // a let nobody reads goes, its division with it
    std::string unread = With("(fn f ((x u8)) u8 (let d u8 (/ x 3)) (let e u16 (* gw 7)) x)");
    ExpectCount(results, unread, "t.f", ExprKind::Let, 0);
    ExpectCount(results, unread, "t.f", ExprKind::Binary, 0);
    CheckProgram(results, unread, "t.f", {200});
    // a let whose value calls something that writes: the call stays
    std::string call = With("(fn bump () u8 (set g (+ g 1)) g)\n"
                            "(fn f () u8 (let d u8 (bump)) (let e u8 (bump)) g)");
    ExpectCount(results, call, "t.f", ExprKind::Let, 0);
    ExpectCount(results, call, "t.f", ExprKind::Call, 2);
    CheckProgram(results, call, "t.f", {});
    // read later: kept
    ExpectCount(results, With("(fn f ((x u8)) u8 (let d u8 (/ x 3)) (+ d 1))"), "t.f",
                ExprKind::Let, 1);
    // a local whose address is taken is read through the pointer
    std::string pointed = With("(fn f () u8 (let d u8 5) (let p (ptr u8) (addr_of d)) (load p))");
    ExpectCount(results, pointed, "t.f", ExprKind::Let, 2);
    CheckProgram(results, pointed, "t.f", {});
}

void DeadStores(TestResults& results) {
    // a local overwritten before any read
    std::string local = With("(fn f ((x u8)) u8 (let y u8 1) (set y (/ x 5)) (set y (+ x 2)) y)");
    ExpectCount(results, local, "t.f", ExprKind::Set, 1);
    CheckProgram(results, local, "t.f", {77});
    // a global overwritten before any read, and one read in between
    std::string global = With("(fn f ((x u8)) void (set g x) (set g (+ x 1)) (set h x)\n"
                              "    (set gw (as u16 h)) (set h 3))");
    ExpectCount(results, global, "t.f", ExprKind::Set, 4);
    CheckProgram(results, global, "t.f", {9});
    // the last store of a global is live when the function returns
    ExpectCount(results, With("(fn f ((x u8)) void (set g x))"), "t.f", ExprKind::Set, 1);
    // and when it may return early
    std::string early = With("(fn f ((x u8)) void (set g 1) (if (> x 3) (return)) (set g 2))");
    ExpectCount(results, early, "t.f", ExprKind::Set, 2);
    CheckProgram(results, early, "t.f", {4});
    // a callee that reads g sees the first store; one that only writes it does not
    std::string reader = With("(fn read_g () u8 g)\n"
                              "(fn f () u8 (set g 1) (let r u8 (read_g)) (set g 2) r)");
    ExpectCount(results, reader, "t.f", ExprKind::Set, 2);
    CheckProgram(results, reader, "t.f", {});
    std::string writer = With("(fn write_g () void (set g 9) (set h 1))\n"
                              "(fn f () u8 (set g 1) (write_g) (set g 2) h)");
    ExpectCount(results, writer, "t.f", ExprKind::Set, 1);
    CheckProgram(results, writer, "t.f", {});
    // a store read through a pointer, in an array whose address is taken
    std::string pointer = With("(fn f () u8 (let p (ptr u8) (addr_of (at a 2)))\n"
                               "    (set (at a 2) 5) (let v u8 (load p)) (set (at a 2) 6) v)");
    ExpectCount(results, pointer, "t.f", ExprKind::Set, 2);
    CheckProgram(results, pointer, "t.f", {});
    // stores to an :address let (screen memory, soft switches) all stay
    ExpectCount(results, With("(fn f () void (set scr 1) (set scr 2) (set scr scr))"), "t.f",
                ExprKind::Set, 3);
    // I/O and jsr in between keep a store, and are never removed themselves
    std::string io = With("(fn f () void (set g 1) (io_write u8 0xC030 0) (set g 2)\n"
                          "    (io_read u8 0xC000) (set h 1) (jsr 0xFBDD) (set h 2))");
    ExpectCount(results, io, "t.f", ExprKind::Set, 4);
    ExpectCount(results, io, "t.f", ExprKind::IoWrite, 1);
    ExpectCount(results, io, "t.f", ExprKind::IoRead, 1);
    ExpectCount(results, io, "t.f", ExprKind::Jsr, 1);
    // what goes round a loop is read on the next pass
    std::string carried = With("(fn f () u8 (let x u8 0) (let s u8 0) (let c u8 0)\n"
                               "    (while (< c 5) (set c (+ c 1)) (set s (+ s x)) (set x c)) s)");
    ExpectCount(results, carried, "t.f", ExprKind::Set, 3);
    CheckProgram(results, carried, "t.f", {});
    // a store at the end of a pass that the next pass's let overwrites
    std::string relet = With("(fn f () u8 (let s u8 0) (for i u8 0 4 (let x u8 i)\n"
                             "    (set s (+ s x)) (set x 9)) s)");
    ExpectCount(results, relet, "t.f", ExprKind::Set, 1);
    CheckProgram(results, relet, "t.f", {});
    // a store before a loop that may never end stays: memory can be looked at meanwhile
    ExpectCount(results,
                With("(fn f () void (set g 1) (let c u8 1) (while (< c 254) (set c (+ c 4)))\n"
                     "    (set g 2))"),
                "t.f", ExprKind::Set, 3);
}

void SelfAndStatements(TestResults& results) {
    std::string self = With("(fn f ((x u8)) u8 (set g g) (set (at a (& x 7)) (at a (& x 7)))\n"
                            "    (set x x) (+ g x))");
    ExpectCount(results, self, "t.f", ExprKind::Set, 0);
    CheckProgram(results, self, "t.f", {3});
    // statements with no effect, a pure call among them
    std::string pure = With("(fn sq ((v u8)) u8 (* v v))\n"
                            "(fn f ((x u8)) u8 (sq x) (if (> x 2) (at a 1) (/ x 3))\n"
                            "    (match x (1 (sq 2)) (_ (do))) x)");
    ExpectCount(results, pure, "t.f", ExprKind::Call, 0);
    ExpectCount(results, pure, "t.f", ExprKind::If, 0);
    ExpectCount(results, pure, "t.f", ExprKind::Match, 0);
    // a call that writes, or that may never come back, stays
    std::string kept = With("(fn bump () u8 (set g (+ g 1)) g)\n"
                            "(fn spin () u8 (let c u8 1) (while (< c 254) (set c (+ c 4))) c)\n"
                            "(fn f () u8 (bump) (spin) 0)");
    ExpectCount(results, kept, "t.f", ExprKind::Call, 2);
}

void Loops(TestResults& results) {
    // race seed 21258's tail: loops around an empty counting while and a let nobody reads
    std::string nested = With("(fn f () u8\n"
                              "    (for n i16 -4 3 (for m u8 (& (at a 1) 3) 6 (for w u16 89 305\n"
                              "        (let j u8 0) (while (< j 1) (set j (+ j 1)))\n"
                              "        (let l i8 (+ (>> (as i8 g) 5) (~ (as i8 (at a 2))))))))\n"
                              "    g)");
    ExpectCount(results, nested, "t.f", ExprKind::For, 0);
    ExpectCount(results, nested, "t.f", ExprKind::While, 0);
    CheckProgram(results, nested, "t.f", {});
    // counting down, signed counters, a bound in a local, steps of more than one
    std::string ends = With("(fn f ((x u8)) u8 (let c u8 x) (while (> c 0) (set c (- c 1)))\n"
                            "    (let d i8 -100) (while (< d 100) (set d (+ d 1)))\n"
                            "    (let k u8 (& x 31)) (let e u8 0) (while (< e k) (set e (+ e 1)))\n"
                            "    (let s u8 0) (while (<= s 200) (set s (+ s 50)))\n"
                            "    (let o u8 x) (while (!= o 7) (set o (+ o 3)))\n"
                            "    x)");
    ExpectCount(results, ends, "t.f", ExprKind::While, 0);
    CheckProgram(results, ends, "t.f", {40});
    // a for whose only write is a global the next statement overwrites
    std::string global = With("(fn f () void (for i u8 0 10 (set g i)) (set g 0))");
    ExpectCount(results, global, "t.f", ExprKind::For, 0);
    CheckProgram(results, global, "t.f", {});
    // effects outside the loop keep it: a store through a pointer, a global read later, I/O,
    // a local read later, a return
    const char* kept[] = {
        "(fn f ((x u8)) u8 (let p (ptr u8) (addr_of (at a 0))) (for i u8 0 4 (store p i)) x)",
        "(fn f ((x u8)) u8 (for i u8 0 4 (set g i)) g)",
        "(fn f ((x u8)) u8 (for i u8 0 4 (io_write u8 0xC030 x)) 0)",
        "(fn f ((x u8)) u8 (let s u8 0) (for i u8 0 4 (set s (+ s i))) s)",
        "(fn f ((x u8)) u8 (for i u8 0 4 (if (= i x) (return 1))) 0)",
        "(fn f ((x u8)) u8 (for i u8 0 4 (set (at a i) x)) 0)",
    };
    for (const char* text : kept) {
        ExpectCount(results, With(text), "t.f", ExprKind::For, 1);
        CheckProgram(results, With(text), "t.f", {2});
    }
}

// loops that might not end stay, and on the 6502 they still do not end
void EndlessLoops(TestResults& results) {
    const char* endless[] = {
        "(let c u8 1) (while (< c 254) (set c (+ c 4)))",  // 253 + 4 wraps to 1
        "(let c u8 0) (while (!= c 3) (set c (+ c 2)))",   // even steps miss 3
        "(let c u8 5) (while (>= c 0) (set c (- c 1)))",   // a byte is always >= 0
        "(let c i8 0) (while (< c 127) (set c (+ c 2)))",  // 126 + 2 wraps to -128
        "(let c u8 0) (while (<= c 255) (set c (+ c 1)))", // always true
        "(let c u8 0) (while (< c 5) (if (< c 9) (continue)) (set c (+ c 1)))",
        "(let c u8 0) (while (< c 5) (if (> c 9) (set c (+ c 1))))",
        "(let c u8 0) (while true (set c (+ c 1)))",
    };
    for (const char* body : endless) {
        std::string text = With(std::string("(fn f () void ") + body + ")");
        ExpectCount(results, text, "t.f", ExprKind::While, 1);
        Compiled c = CompileText(text);
        Diff d;
        bool started = StartDiff(d, c);
        CHECK(results, started);
        if (!started) {
            continue;
        }
        std::int32_t f = tisple::FindFunction(c.built.program, "t.f");
        tisple::SimCall got = tisple::CallCompiled(d.sim, c.built.program, c.code, f, {}, 200000);
        if (got.returned) {
            std::fprintf(stderr, "an endless loop ended on the 6502:\n%s\n", text.c_str());
        }
        CHECK(results, !got.returned);
    }
    // a while true that a break ends also counts as possibly endless
    ExpectCount(results,
                With("(fn f () void (let c u8 0) (while true (set c (+ c 1)) "
                     "(if (= c 10) (break))))"),
                "t.f", ExprKind::While, 1);
}

// a node kind the passes do not know (a new effect) counts as every effect: the store before
// it stays, a loop around it stays, a call to its function stays
void UnknownNodes(TestResults& results) {
    std::string text = With("(fn mark () void (set h 77))\n"
                            "(fn f () void (set g 1) (set h 77) (set g 2)\n"
                            "    (for i u8 0 3 (set h 77)) (mark))");
    Built built = BuildText(text);
    CHECK(results, built.ok);
    if (!built.ok) {
        return;
    }
    tisple::Program program = built.program;
    int replaced = 0;
    for (tisple::Expr& e : program.exprs) {
        if (e.kind == ExprKind::Set && e.b >= 0 &&
            program.exprs[static_cast<std::size_t>(e.b)].kind == ExprKind::Literal &&
            program.exprs[static_cast<std::size_t>(e.b)].value == 77) {
            e.kind = static_cast<ExprKind>(200);
            replaced++;
        }
    }
    CHECK(results, replaced == 3);
    tisple::OptReport report;
    tisple::OptimizeProgram(program, tisple::OptOptions(), report);
    Opted o;
    o.ok = true;
    o.program = program;
    CHECK(results, CountKind(o, "t.f", ExprKind::Set) == 2);
    CHECK(results, CountKind(o, "t.f", ExprKind::For) == 1);
    CHECK(results, CountKind(o, "t.f", ExprKind::Call) == 1);
    CHECK(results, CountKind(o, "t.f", static_cast<ExprKind>(200)) == 2);
}

} // namespace

void RunOptTests(TestResults& results) {
    DeadLets(results);
    DeadStores(results);
    SelfAndStatements(results);
    Loops(results);
    EndlessLoops(results);
    UnknownNodes(results);
    RunOptFlagTests(results);
}

} // namespace tisple_tests
