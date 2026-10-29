#include "test_check.hpp"

#include <algorithm>
#include <cstdio>

namespace tisple_tests {

namespace {

void OperatorTests(TestResults& results) {
    // no implicit widening; the fix widens the narrower side
    ExpectError(results, "(fn f ((a u8) (b u16)) u16 (+ a b))",
                "t.tsp:1:28: u8 + u16: types must match; write (+ (as u16 a) b)");
    ExpectError(results, "(fn f ((a i16) (b u8)) bool (< a b))",
                "t.tsp:1:29: i16 < u8: types must match; write (< a (as i16 b))");
    ExpectError(results, "(fn f ((x u8)) u8 (+ x 300))",
                "t.tsp:1:24: 300 does not fit in u8; write (+ (as u16 x) 300)");
    ExpectError(results, "(fn f ((x u8)) u8 (- x -1))",
                "t.tsp:1:24: -1 does not fit in u8; write (- (as i16 x) -1)");
    ExpectError(results, "(fn f ((b bool)) u8 (+ b 1))",
                "t.tsp:1:21: + works on integers, not bool");
    ExpectError(results, "(fn f ((p (ptr u8))) u16 (& p 1))",
                "t.tsp:1:26: & works on integers, not a pointer");
    ExpectError(results, "(enum A (one))\n(fn f ((a A)) bool (< a A.one))",
                "t.tsp:2:20: < works on integers, not the enum A");
    ExpectError(results, "(fn f ((a u8) (b u8) (c u8)) u8 (+ a b c))",
                "t.tsp:1:33: + takes two operands; write (+ (+ a b) c)");
    ExpectError(results, "(fn f ((x u8)) bool (not x))",
                "t.tsp:1:21: not works on bool, not u8; write (= x 0)");
    ExpectError(results, "(fn f ((x u8)) bool (and x true))",
                "t.tsp:1:26: and needs a bool but x is u8; write (!= x 0)");
    // pointer arithmetic moves by elements, with a u8 or u16 count
    ExpectError(results, "(fn f ((p (ptr u8)) (n i8)) (ptr u8) (+ p n))",
                "t.tsp:1:43: a pointer moves by a u8 or u16 count of elements; n is i8; write "
                "(+ p (as u16 n))");
    ExpectError(results, "(fn f ((p (ptr u8)) (n i16)) (ptr u8) (- p n))",
                "t.tsp:1:44: a pointer moves by a u8 or u16 count of elements; n is i16; write "
                "(- p (as u16 n))");
    ExpectError(results, "(fn f ((p (ptr u8))) (ptr u8) (+ 1 p))",
                "t.tsp:1:31: the pointer goes first; write (+ p 1)");
}

// (+ p x) with x : u8 is the tree (+ p (as u16 x)) gives: a Convert to u16 under the PtrAdd
void PointerCountTests(TestResults& results) {
    Built built = BuildText("(fn f ((p (ptr u16)) (x u8)) (ptr u16) (- (+ p x) (+ x 1)))");
    CHECK(results, built.ok);
    int widened = 0;
    for (const tisple::Expr& expr : built.program.exprs) {
        if (expr.kind != tisple::ExprKind::PtrAdd && expr.kind != tisple::ExprKind::PtrSub) {
            continue;
        }
        const tisple::Expr& count = built.program.exprs[static_cast<std::size_t>(expr.b)];
        const tisple::Expr& inner = built.program.exprs[static_cast<std::size_t>(count.a)];
        bool ok = count.kind == tisple::ExprKind::Convert && count.type == tisple::kU16Type &&
                  inner.type == tisple::kU8Type;
        widened += ok ? 1 : 0;
    }
    CHECK(results, widened == 2);
    // the u8 sum wraps before it widens, as in (as u16 (+ x 1))
    tisple::Interp in;
    StartInterp(in, built);
    CHECK(results, CallOk(results, in, "t.f", {0x1000, 255}) == 0x1000 + 2 * 255);
    CHECK(results, CallOk(results, in, "t.f", {0x1000, 3}) == 0x1000 - 2);
}

// a literal in a value slot takes the slot's type
void ValueSlotTests(TestResults& results) {
    Built built = BuildText("(struct S ((x u8) (y u16)))\n"
                            "(enum E (a b))\n"
                            "(let g (array u8 4))\n"
                            "(let s S)\n"
                            "(let t (array u16 2) (make (array u16 2) 1 0x1234))\n"
                            "(fn h ((a u8) (b u16) (e E)) u16\n"
                            "    (if (= a 0)\n"
                            "        (return 0x1234))\n"
                            "    b)\n"
                            "(fn f ((x u8)) void\n"
                            "    (let p (ptr u8) (addr_of (at g 0)))\n"
                            "    (let q (ptr S) (addr_of s))\n"
                            "    (store p 0)\n"
                            "    (store (+ p x) 0xFF)\n"
                            "    (store q y 300)\n"
                            "    (store q (make S :x 1 :y 2))\n"
                            "    (set s (make S :x 1 :y 0x1234))\n"
                            "    (io_write u16 0xC030 0)\n"
                            "    (h 1 2 E.b)\n"
                            "    (match (load p)\n"
                            "        (1 (store p 2))\n"
                            "        (_)))");
    CHECK(results, built.ok);
    if (!built.ok) {
        std::fprintf(stderr, "%s", ErrorText(built).c_str());
    }
    // a bad pointer or place is the only error: its literal value does not add one
    Built bad = BuildText("(fn f ((p (ptr u8)) (n i8)) void\n"
                          "    (store (+ p n) 0)\n"
                          "    (set nope 1)\n"
                          "    (store nope 2)\n"
                          "    (let v u8 (as Nope 3)))");
    std::string errors = ErrorText(bad);
    CHECK(results, errors.find("no type") == std::string::npos);
    CHECK(results, std::count(errors.begin(), errors.end(), '\n') == 4);
    if (std::count(errors.begin(), errors.end(), '\n') != 4) {
        std::fprintf(stderr, "%s", errors.c_str());
    }
}

void LiteralTests(TestResults& results) {
    ExpectError(results, "(let x u8 256)", "t.tsp:1:11: 256 does not fit in u8 (0 to 255)");
    ExpectError(results, "(let x i8 128)", "t.tsp:1:11: 128 does not fit in i8 (-128 to 127)");
    ExpectError(results, "(fn f () void (let n 5))",
                "t.tsp:1:22: (let n 5): let always states the type; write (let n u8 5)");
    ExpectError(results, "(let n 5)",
                "t.tsp:1:8: (let n 5): let always states the type; write (let n u8 5)");
    ExpectError(results, "(const N 5)",
                "t.tsp:1:10: (const N 5): const always states the type; write (const N u8 5)");
    ExpectError(results, "(fn f () bool (= 1 2))",
                "t.tsp:1:15: (= 1 2): neither side has a type; a literal takes the type of what "
                "it meets, so use a typed value on one side");
    ExpectError(
        results, "(fn f () u16 (as u16 5))",
        "t.tsp:1:14: (as u16 5) converts nothing: a literal takes its type from where it is "
        "used; write 5");
    ExpectError(results, "(fn f () void 5 (do))",
                "t.tsp:1:15: 5 on its own does nothing; remove it");
    ExpectError(results, "(fn f () bool 1)", "t.tsp:1:15: 1 is not a bool; write true or false");
    ExpectError(results, "(fn f () (ptr u8) 0x2000)",
                "t.tsp:1:19: a pointer is not a number; write (as (ptr u8) 0x2000)");
    ExpectError(results, "(enum A (one))\n(fn f () A 0)",
                "t.tsp:2:12: 0 is not a A; name a member, as in A.one");
    ExpectError(results, "(fn f () void \"hi\")",
                "t.tsp:1:15: \"hi\" on its own does nothing; remove it");
}

void ConversionTests(TestResults& results) {
    // as is the only conversion: integers, enum members to their number, u16 and pointers
    Built conversions = BuildText("(enum A ((one 1) (two 200)))\n"
                                  "(fn f ((p (ptr u8)) (x i8)) u16\n"
                                  "    (+ (as u16 p) (+ (as u16 x) (as u16 A.two))))");
    CHECK(results, conversions.ok);
    ExpectError(results, "(fn f ((p (ptr u8))) u8 (as u8 p))",
                "t.tsp:1:25: a pointer converts only to u16; write (as u8 (as u16 p))");
    ExpectError(results, "(fn f ((b bool)) u8 (as u8 b))",
                "t.tsp:1:21: as converts integers, not bool; write (if b 1 0)");
    ExpectError(results, "(fn f ((x u8)) bool (as bool x))",
                "t.tsp:1:21: nothing converts to bool; write (!= x 0)");
    ExpectError(results, "(fn f ((x u16)) u16 (as u16 x 1))",
                "t.tsp:1:21: as takes a type and a value: (as u16 x), (as u8 Action.fire), "
                "(as (ptr u8) addr) or (as u16 p)");
    ExpectError(results, "(fn f ((x u8)) (ptr u8) (as (ptr u8) x))",
                "t.tsp:1:38: (as (ptr u8) ...) needs a u16 but x is u8; write (as u16 x)");
    ExpectError(results, "(fn f () void (io_read u32 0xC000))",
                "t.tsp:1:24: io_read moves u8 or u16 (low byte first); write (io_read u8 addr)");
    ExpectError(results, "(fn f ((v u16)) void (io_write u8 0xC030 v))",
                "t.tsp:1:42: the io_write value needs a u8 but v is u16; write (as u8 v)");
    ExpectError(results, "(fn f ((a u8)) void (io_read u8 a))",
                "t.tsp:1:33: the io_read address needs a u16 but a is u8; write (as u16 a)");
}

void CallTests(TestResults& results) {
    ExpectError(results, "(fn g ((x u8)) void)\n(fn f () void (g))",
                "t.tsp:2:15: g takes 1 argument, got 0; write (g x)");
    ExpectError(results, "(fn g ((x u8)) void)\n(fn f ((y u16)) void (g y))",
                "t.tsp:2:25: argument x of g needs a u8 but y is u16; write (as u8 y)");
    ExpectError(results, "(fn g () void)\n(fn f () u8 (g))",
                "t.tsp:2:13: f returns u8 but its last form gives no value; end the body with a u8 "
                "value or (return v)");
    ExpectError(results, "(struct P ((x u8)))\n(fn f ((p P)) void)",
                "t.tsp:2:11: parameters are 1 or 2 bytes (integers, bool, enums, pointers); pass "
                "a (ptr P) instead");
    ExpectError(results, "(struct P ((x u8)))\n(fn f () P)",
                "t.tsp:2:10: a function returns 1 or 2 bytes (integers, bool, enums, pointers); "
                "return a (ptr P) instead");
    ExpectError(results, "(fn main ((x u8)) void)",
                "t.tsp:1:1: main takes no parameters; write (fn main () void ...)");
    ExpectError(results, "(fn f () void (g))\n(fn g () void (f))",
                "t.tsp:1:1: t.f calls itself through t.g -> t.f; recursion is not in this "
                "subset, write it as a loop");
    ExpectError(results, "(fn f () void (f))",
                "t.tsp:1:1: t.f calls itself; recursion is not in this subset, write it as a loop");
}

void StructTests(TestResults& results) {
    const std::string p = "(struct P ((x u8) (y u8)))\n";
    ExpectError(results, p + "(let a P (make P :y 1 :x 2))",
                "t.tsp:2:10: (make P :y 1 :x 2) fields out of order; write (make P :x 2 :y 1)");
    ExpectError(results, p + "(let a P (make P :x 1))",
                "t.tsp:2:10: (make P :x 1) is missing y; name every field in order: (make P :x 1 "
                ":y ...)");
    ExpectError(results, p + "(let a P (make P :x 1 :z 2))",
                "t.tsp:2:23: P has no field z; its fields are x y");
    ExpectError(results, p + "(let a P (make P :x 1 :x 2))",
                "t.tsp:2:23: :x is given twice; give each field once");
    ExpectError(results, p + "(let a P (make P 1 2))",
                "t.tsp:2:18: struct literals name every field: write (make P :x value ...)");
    ExpectError(results, p + "(let a P (make P :x 1 :y 300))",
                "t.tsp:2:26: 300 does not fit in u8 (0 to 255)");
    ExpectError(results, "(struct A ((b B)))\n(struct B ((a A)))",
                "t.tsp:2:12: struct A contains itself; use a (ptr A) field");
    ExpectError(results, "(struct E)",
                "t.tsp:1:1: a struct needs fields; write (struct E ((x u8) (y u8)))");
    ExpectError(results, p + "(let a P)\n(fn f ((c bool)) void (set a (if c a a)))",
                "t.tsp:3:30: a struct value comes from (make ...), a variable, (at ...), a field "
                "or a load; set each case on its own instead, as in (if c (set s a) (set s b))");
    ExpectError(results, p + "(let a P)\n(fn f () bool (= a a))",
                "t.tsp:3:15: = compares integers, bools, enum members and pointers, not P");
}

} // namespace

void RunCheckTypeTests(TestResults& results) {
    OperatorTests(results);
    PointerCountTests(results);
    ValueSlotTests(results);
    LiteralTests(results);
    ConversionTests(results);
    CallTests(results);
    StructTests(results);
}

} // namespace tisple_tests
