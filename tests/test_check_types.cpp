#include "test_check.hpp"

namespace tisple_tests {

namespace {

void OperatorTests(TestResults& results) {
    // no implicit widening; the fix widens the narrower side
    ExpectError(results, "(fn f (args (a u8) (b u16)) (returns u16) (+ a b))",
                "t.tsp:1:43: u8 + u16: types must match; write (+ (u16 a) b)");
    ExpectError(results, "(fn f (args (a i16) (b u8)) (returns bool) (< a b))",
                "t.tsp:1:44: i16 < u8: types must match; write (< a (i16 b))");
    ExpectError(results, "(fn f (args (x u8)) (returns u8) (+ x 300))",
                "t.tsp:1:39: 300 does not fit in u8; write (+ (u16 x) 300)");
    ExpectError(results, "(fn f (args (x u8)) (returns u8) (- x -1))",
                "t.tsp:1:39: -1 does not fit in u8; write (- (i16 x) -1)");
    ExpectError(results, "(fn f (args (b bool)) (returns u8) (+ b 1))",
                "t.tsp:1:36: + works on integers, not bool");
    ExpectError(results, "(fn f (args (p (ptr u8))) (returns u16) (& p 1))",
                "t.tsp:1:41: & works on integers, not a pointer");
    ExpectError(results, "(enum A one)\n(fn f (args (a A)) (returns bool) (< a A.one))",
                "t.tsp:2:35: < works on integers, not the enum A");
    ExpectError(results, "(fn f (args (a u8) (b u8) (c u8)) (returns u8) (+ a b c))",
                "t.tsp:1:48: + takes two operands; write (+ (+ a b) c)");
    ExpectError(results, "(fn f (args (x u8)) (returns bool) (not x))",
                "t.tsp:1:36: not works on bool, not u8; write (= x 0)");
    ExpectError(results, "(fn f (args (x u8)) (returns bool) (and x true))",
                "t.tsp:1:41: and needs a bool but x is u8; write (!= x 0)");
    // pointer arithmetic moves by elements, with a u16 count
    ExpectError(results, "(fn f (args (p (ptr u8)) (n u8)) (returns (ptr u8)) (+ p n))",
                "t.tsp:1:58: a pointer moves by a u16 count of elements; write (+ p (u16 n))");
    ExpectError(results, "(fn f (args (p (ptr u8))) (returns (ptr u8)) (+ 1 p))",
                "t.tsp:1:46: the pointer goes first; write (+ p 1)");
}

void LiteralTests(TestResults& results) {
    ExpectError(results, "(let x u8 256)", "t.tsp:1:11: 256 does not fit in u8 (0 to 255)");
    ExpectError(results, "(let x i8 128)", "t.tsp:1:11: 128 does not fit in i8 (-128 to 127)");
    ExpectError(results, "(fn f (let n 5))",
                "t.tsp:1:14: (let n 5): the literal has no type; write (let n u8 5)");
    ExpectError(results, "(let n 5)",
                "t.tsp:1:8: (let n 5): the literal has no type; write (let n u8 5)");
    ExpectError(results, "(const N 5)",
                "t.tsp:1:10: (const N 5): the literal has no type; write (const N u8 5)");
    ExpectError(results, "(fn f (returns bool) (= 1 2))",
                "t.tsp:1:22: (= 1 2): neither side has a type; a literal takes the type of what "
                "it meets, so use a typed value on one side");
    ExpectError(results, "(fn f (returns u16) (u16 5))",
                "t.tsp:1:21: (u16 5) converts nothing: a literal takes its type from where it is "
                "used; write 5");
    ExpectError(results, "(fn f 5 (do))", "t.tsp:1:7: 5 on its own does nothing; remove it");
    ExpectError(results, "(fn f (returns bool) 1)",
                "t.tsp:1:22: 1 is not a bool; write true or false");
    ExpectError(results, "(fn f (returns (ptr u8)) 0x2000)",
                "t.tsp:1:26: a pointer is not a number; write (as (ptr u8) 0x2000)");
    ExpectError(results, "(enum A one)\n(fn f (returns A) 0)",
                "t.tsp:2:19: 0 is not a A; name a member, as in A.one");
    ExpectError(results, "(fn f \"hi\")", "t.tsp:1:7: strings are not in this subset yet");
}

void ConversionTests(TestResults& results) {
    ExpectError(results, "(fn f (args (p (ptr u8))) (returns u16) (u16 p))",
                "t.tsp:1:41: u16 converts integers; for a pointer write (as u16 p)");
    ExpectError(results, "(fn f (args (b bool)) (returns u8) (u8 b))",
                "t.tsp:1:36: u8 converts integers, not bool; write (if b 1 0)");
    ExpectError(results, "(fn f (args (x u8)) (returns u16) (as u16 x))",
                "t.tsp:1:35: (as u16 x) takes a pointer; for an integer write (u16 x)");
    ExpectError(results, "(fn f (args (x u16)) (returns u8) (as u8 x))",
                "t.tsp:1:35: as converts between u16 and pointers: (as (ptr u8) n) or (as u16 p); "
                "for integers write (u8 x)");
    ExpectError(results, "(fn f (args (x u8)) (returns (ptr u8)) (as (ptr u8) x))",
                "t.tsp:1:53: (as (ptr u8) ...) needs a u16 but x is u8; write (u16 x)");
    ExpectError(results, "(fn f (io_read u32 0xC000))",
                "t.tsp:1:16: io_read moves u8 or u16 (low byte first); write (io_read u8 addr)");
    ExpectError(results, "(fn f (args (v u16)) (io_write u8 0xC030 v))",
                "t.tsp:1:42: the io_write value needs a u8 but v is u16; write (u8 v)");
    ExpectError(results, "(fn f (args (a u8)) (io_read u8 a))",
                "t.tsp:1:33: the io_read address needs a u16 but a is u8; write (u16 a)");
}

void CallTests(TestResults& results) {
    ExpectError(results, "(fn g (args (x u8)))\n(fn f (g))",
                "t.tsp:2:7: g takes 1 argument, got 0; write (g x)");
    ExpectError(results, "(fn g (args (x u8)))\n(fn f (args (y u16)) (g y))",
                "t.tsp:2:25: argument x of g needs a u8 but y is u16; write (u8 y)");
    ExpectError(results, "(fn g)\n(fn f (returns u8) (g))",
                "t.tsp:2:20: f returns u8 but its last form gives no value; end the body with a u8 "
                "value or (return v)");
    ExpectError(results, "(struct P (x u8))\n(fn f (args (p P)))",
                "t.tsp:2:16: parameters are 1 or 2 bytes (integers, bool, enums, pointers); pass "
                "a (ptr P) instead");
    ExpectError(results, "(struct P (x u8))\n(fn f (returns P))",
                "t.tsp:2:16: a function returns 1 or 2 bytes (integers, bool, enums, pointers); "
                "return a (ptr P) instead");
    ExpectError(results, "(fn f (args))", "t.tsp:1:7: omit (args) when f takes no arguments");
    ExpectError(results, "(fn main (args (x u8)))",
                "t.tsp:1:1: main takes no arguments; remove its (args ...)");
    ExpectError(results, "(fn f (g))\n(fn g (f))",
                "t.tsp:1:1: t.f calls itself through t.g -> t.f; recursion is not in this "
                "subset, write it as a loop");
    ExpectError(results, "(fn f (f))",
                "t.tsp:1:1: t.f calls itself; recursion is not in this subset, write it as a loop");
}

void StructTests(TestResults& results) {
    const std::string p = "(struct P (x u8) (y u8))\n";
    ExpectError(results, p + "(let a (P :y 1 :x 2))",
                "t.tsp:2:8: (P :y 1 :x 2) fields out of order; write (P :x 2 :y 1)");
    ExpectError(results, p + "(let a (P :x 1))",
                "t.tsp:2:8: (P :x 1) is missing y; name every field in order: (P :x 1 :y ...)");
    ExpectError(results, p + "(let a (P :x 1 :z 2))",
                "t.tsp:2:16: P has no field z; its fields are x y");
    ExpectError(results, p + "(let a (P :x 1 :x 2))",
                "t.tsp:2:16: :x is given twice; give each field once");
    ExpectError(results, p + "(let a (P 1 2))",
                "t.tsp:2:11: struct literals name every field: write (P :x value ...)");
    ExpectError(results, p + "(let a (P :x 1 :y 300))",
                "t.tsp:2:19: 300 does not fit in u8 (0 to 255)");
    ExpectError(results, "(struct A (b B))\n(struct B (a A))",
                "t.tsp:2:11: struct A contains itself; use a (ptr A) field");
    ExpectError(results, "(struct E)",
                "t.tsp:1:1: a struct needs fields; write (struct E (x u8) (y u8))");
    ExpectError(results, p + "(let a P)\n(fn f (args (c bool)) (set a (if c a a)))",
                "t.tsp:3:30: a struct value comes from a struct literal, a variable, (at ...), a "
                "field or (load p); set each case on its own instead, as in (if c (set s a) (set "
                "s b))");
    ExpectError(results, p + "(let a P)\n(fn f (returns bool) (= a a))",
                "t.tsp:3:22: = compares integers, bools, enum members and pointers, not P");
}

} // namespace

void RunCheckTypeTests(TestResults& results) {
    OperatorTests(results);
    LiteralTests(results);
    ConversionTests(results);
    CallTests(results);
    StructTests(results);
}

} // namespace tisple_tests
