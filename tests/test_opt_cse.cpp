// common subexpressions (src/opt/cse.cpp): a value worked out again while it holds is read
// from a local, and what must be worked out again because something in between may change
// what it reads; each program also run on the 6502 against the interpreter, which runs it as
// written

#include "test_backend.hpp"
#include "test_opt.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

using tisple::ExprKind;
using tisple::Op;

const char* const kGlobals = "(let g u16 0)\n(let h u16 0)\n(let gw u16 0)\n(let gb u8 0)\n"
                             "(let aw (array u16 8))\n(let scr u16 :address 0x0400)\n";

std::string With(const std::string& text) {
    return std::string(kGlobals) + text;
}

// binaries of op under a function's body after the passes
int CountOp(const Opted& o, const std::string& function, Op op) {
    std::int32_t f = tisple::FindFunction(o.program, function);
    if (f < 0) {
        return -1;
    }
    int count = 0;
    std::vector<tisple::ExprId> work = {o.program.functions[static_cast<std::size_t>(f)].body};
    while (!work.empty()) {
        tisple::ExprId id = work.back();
        work.pop_back();
        if (id < 0) {
            continue;
        }
        const tisple::Expr& e = o.program.exprs[static_cast<std::size_t>(id)];
        count += e.kind == ExprKind::Binary && e.op == op ? 1 : 0;
        for (tisple::ExprId child : {e.a, e.b, e.c}) {
            work.push_back(child);
        }
        if (e.kind == ExprKind::Call || e.kind == ExprKind::Do) {
            for (std::uint32_t i = 0; i < e.count; i++) {
                work.push_back(o.program.lists[e.first + i]);
            }
        }
        if (e.kind == ExprKind::Match) {
            for (std::uint32_t i = 0; i < e.count; i++) {
                work.push_back(o.program.clauses[e.first + i].body);
            }
        }
    }
    return count;
}

void ExpectOps(TestResults& results, const std::string& text, Op op, int want,
               const std::vector<std::uint16_t>& args) {
    Opted o = Optimize(text, NoUnroll());
    int got = CountOp(o, "t.f", op);
    if (got != want) {
        std::fprintf(stderr, "t.f has %d of op %d, not %d:\n%s\n", got, static_cast<int>(op), want,
                     text.c_str());
    }
    CHECK(results, o.ok && got == want);
    CheckProgram(results, text, "t.f", args);
}

void Shares(TestResults& results) {
    // two statements in a row
    ExpectOps(results,
              With("(fn f ((x u16) (y u16)) u16\n"
                   "    (set g (+ (* x y) 1)) (set h (- (* x y) 2)) (+ g h))"),
              Op::Mul, 1, {300, 7});
    // read again in an arm, in a loop that leaves its inputs alone, and in a call's argument
    ExpectOps(results,
              With("(fn id ((v u16)) u16 (set gw v) v)\n"
                   "(fn f ((x u16) (y u16)) u16 (set g (/ x y))\n"
                   "    (if (> x 9) (set h (/ x y)))\n"
                   "    (for i u8 0 (as u8 y) (set (at aw (& i 7)) (+ (/ x y) (as u16 i))))\n"
                   "    (+ (id (/ x y)) g))"),
              Op::Div, 1, {1000, 7});
    // worked out twice in one loop pass, from a counter that changes on every pass
    ExpectOps(results,
              With("(fn f ((x u16)) u16\n"
                   "    (for i u16 0 5 (set g (+ g (* x i))) (set h (^ h (* x i)))) (+ g h))"),
              Op::Mul, 1, {300});
    // twice in one expression, in a void arm that is not a do
    ExpectOps(results,
              With("(fn f ((x u16) (y u16)) u16 (if (> x 3) (set g (+ (% x y) (% x y)))) g)"),
              Op::Mod, 1, {1000, 7});
    // a pure call
    Opted call = Optimize(With("(fn sq ((v u16)) u16 (* v v))\n"
                               "(fn f ((x u16)) u16 (set g (sq x)) (set h (+ (sq x) 1)) h)"));
    CHECK(results, CountKind(call, "t.f", ExprKind::Call) == 1);
    CheckProgram(results,
                 With("(fn sq ((v u16)) u16 (* v v))\n"
                      "(fn f ((x u16)) u16 (set g (sq x)) (set h (+ (sq x) 1)) h)"),
                 "t.f", {300});
    // a part of a bigger value worked out again, and the bigger value too
    ExpectOps(results,
              With("(fn f ((x u16) (y u16)) u16 (set g (+ (* (/ x y) 3) 1))\n"
                   "    (set h (/ x y)) (set gw (+ (* (/ x y) 3) 1)) (+ g gw))"),
              Op::Div, 1, {1000, 7});
}

// a division and the remainder of the same operands, apart: worked out by one division
void Partners(TestResults& results) {
    std::string text = With("(fn f ((x u16) (y u16)) u16 (set g (/ x y))\n"
                            "    (if (> x 9) (set gb 1) (set gb 2)) (set gw (+ gw 1))\n"
                            "    (set h (% x y)) (+ g h))");
    Compiled c = CompileText(text);
    CHECK(results, c.ok && CountMn(c, "t.f", tisple::Mn::JSR) == 1);
    CheckProgram(results, text, "t.f", {1000, 7});
    CheckProgram(results, text, "t.f", {5, 0});
    // a pair side by side the backend shares already, and the quotient again far away: read
    // from a local, one division, no lets of copies made round after round
    std::string both = With("(fn f ((x u16) (y u16)) u16 (set g (/ x y)) (set h (% x y))\n"
                            "    (if (> x 9) (set gb 1) (set gb 2)) (set gw (+ (/ x y) 1)) g)");
    Compiled c2 = CompileText(both);
    CHECK(results, c2.ok && CountMn(c2, "t.f", tisple::Mn::JSR) == 1);
    Opted o = Optimize(both);
    CHECK(results, o.ok && CountKind(o, "t.f", ExprKind::Let) <= 2 && o.report.applied <= 3);
    CheckProgram(results, both, "t.f", {1000, 7});
    // signed, byte-wide, and a divisor of zero
    std::string small = With("(fn f ((x i8) (y i8)) u16 (set gb (as u8 (/ x y)))\n"
                             "    (if (> x 9) (set gw 1)) (set gw (+ gw 1))\n"
                             "    (set g (as u16 (% x y))) g)");
    CheckProgram(results, small, "t.f", {0xF3, 5});
    CheckProgram(results, small, "t.f", {0x83, 0});
}

void Stays(TestResults& results) {
    // something in between may change what the value reads: a set of an input, a call that
    // sets a global it reads, a store through a pointer into an address-taken array it reads,
    // a loop that sets an input, a write earlier in the first
    // statement, io_read, io_write, jsr, wait_cycles, a pure function reading a global set in
    // between; an :address variable (hardware); a value first worked out in an arm; cheap
    // work; an array element
    const char* kept[] = {
        "(fn f ((x u16) (y u16)) u16 (set g (* x y)) (set x 3) (set h (* x y)) (+ g h))",
        "(fn bump () void (set gw (+ gw 1)))\n"
        "(fn f ((x u16) (y u16)) u16 (set g (* gw y)) (bump) (set h (* gw y)) (+ g h))",
        "(fn f ((x u16) (y u16)) u16 (let p (ptr u16) (addr_of (at aw 1)))\n"
        "    (set g (* (at aw 1) y)) (store p 5) (set h (* (at aw 1) y)) (+ g h))",
        "(fn f ((x u16) (y u16)) u16 (set g (* x y))\n"
        "    (for i u16 0 3 (set h (+ h (* x y))) (set x (+ x i))) (+ g h))",
        "(fn f ((x u16) (y u16)) u16 (set g (* x y))\n"
        "    (set h (+ (do (set x 2) 0) (* x y))) (set gw (* x y)) (+ g gw))",
        "(fn f ((x u16) (y u16)) u16 (set g (* x y)) (set gb (io_read u8 0xC000))\n"
        "    (set h (* x y)) (+ g h))",
        "(fn f ((x u16) (y u16)) u16 (set g (* x y)) (io_write u8 0xC030 1)\n"
        "    (set h (* x y)) (+ g h))",
        "(fn f ((x u16) (y u16)) u16 (set g (* gw y)) (jsr 0xFBDD) (set h (* gw y)) (+ g h))",
        "(fn f ((x u16) (y u16)) u16 (set g (* gw y)) (wait_cycles 50) (set h (* gw y))\n"
        "    (+ g h))",
        "(fn rd ((v u16)) u16 (* gw v))\n"
        "(fn f ((x u16) (y u16)) u16 (set g (rd y)) (set gw (+ gw 1)) (set h (rd y)) (+ g h))",
        "(fn f ((x u16) (y u16)) u16 (set g (* scr y)) (set h (* scr y)) (+ g h))",
        "(fn f ((x u16) (y u16)) u16 (if (> x 5) (set g (* x y))) (set h (* x y)) (+ g h))",
        "(fn f ((x u16) (y u16)) u16 (set g (+ x y)) (set h (+ x y)) (+ g h))",
        // work a loop never changes the inputs of, in an arm: code motion's or the backend's
        "(fn f ((x u16) (y u16)) u16\n"
        "    (for i u8 0 5 (if (= (& i 1) 0) (set g (+ g (+ (* x y) (* x y)))))) g)",
        "(fn f ((x u16) (y u16)) u16 (set g (at aw (as u8 (& x 7)))) (set h (at aw (as u8 (& x "
        "7)))) (+ g h))",
    };
    tisple::OptOptions no_cse;
    no_cse.disabled = {"cse", "unroll"};
    for (const char* text : kept) {
        Opted o = Optimize(With(text), NoUnroll());
        int lets = CountKind(o, "t.f", ExprKind::Let);
        int without = CountKind(Optimize(With(text), no_cse), "t.f", ExprKind::Let);
        if (lets != without) {
            std::fprintf(stderr, "kept in a local:\n%s\n", text);
        }
        CHECK(results, o.ok && lets == without);
        // the interpreter stops at a jsr: machine code runs only on the 6502
        if (std::string(text).find("jsr") == std::string::npos) {
            CheckProgram(results, With(text), "t.f", {300, 7});
        }
    }
}

} // namespace

void RunOptCseTests(TestResults& results) {
    Shares(results);
    Partners(results);
    Stays(results);
}

} // namespace tisple_tests
