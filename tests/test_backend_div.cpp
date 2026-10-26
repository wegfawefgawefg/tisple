// division and remainder on the 6502 against the interpreter: every u8 dividend by every
// constant divisor, u16 dividends over the whole range by small and large constants, and
// the / and % pairs that divide once, at the edges of when the first result may be reused

#include "test_backend.hpp"

#include <string>

namespace tisple_tests {

namespace {

// every u8 a: / and % by c as a pair, % first, and each alone
void TestByteConstants(TestResults& results) {
    for (int c = 0; c < 256; c++) {
        std::string k = std::to_string(c);
        std::string text = "(let q (array u8 256)) (let r (array u8 256))\n"
                           "(let q2 (array u8 256)) (let r2 (array u8 256))\n"
                           "(fn f () void\n"
                           "    (for a u8 0 256\n"
                           "        (set (at q a) (/ a " +
                           k + "))\n        (set (at r a) (% a " + k +
                           "))\n"
                           "        (set (at r2 a) (% a " +
                           k + "))\n        (set (at q2 a) (/ a " + k +
                           "))))\n"
                           "(fn alone ((a u8)) u8 (+ (/ a " +
                           k + ") (* (% (+ a 1) " + k + ") 3)))\n";
        CheckProgram(results, text, "t.f", {});
        CheckProgram(results, text, "t.alone", {static_cast<std::uint16_t>(c * 37 % 256)});
    }
}

// u16 dividends: every 109th value through the whole range and 0..299, by c
void TestWordConstant(TestResults& results, int c) {
    std::string k = std::to_string(c);
    std::string text = "(let q (array u16 900)) (let r (array u16 900))\n"
                       "(fn f () void\n"
                       "    (for i u16 0 600\n"
                       "        (let a u16 (* i 109))\n"
                       "        (set (at q i) (/ a " +
                       k + "))\n        (set (at r i) (% a " + k +
                       ")))\n"
                       "    (for i u16 0 300\n"
                       "        (set (at r (+ i 600)) (% i " +
                       k + "))\n        (set (at q (+ i 600)) (/ i " + k +
                       "))))\n"
                       "(fn g ((a u16)) u16 (- (% a " +
                       k + ") (/ a " + k + ")))\n";
    CheckProgram(results, text, "t.f", {});
    const std::uint16_t dividends[] = {0, 1, 6, 7, 255, 256, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF};
    for (std::uint16_t a : dividends) {
        CheckProgram(results, text, "t.g", {a});
    }
}

void TestWordConstants(TestResults& results) {
    for (int c = 0; c <= 20; c++) {
        TestWordConstant(results, c);
    }
    for (int c : {25, 31, 63, 64, 100, 126, 127, 128, 129, 200, 255, 256, 257, 1000, 4097, 0x7FFF,
                  0xFFFF}) {
        TestWordConstant(results, c);
    }
}

// signed and variable divisors still go through the runtime, also as pairs
void TestRuntimePairs(TestResults& results) {
    CheckProgram(results,
                 "(let q (array i8 256)) (let r (array i8 256))\n"
                 "(fn f () void\n"
                 "    (for a u8 0 256\n"
                 "        (let s i8 (as i8 a))\n"
                 "        (set (at q a) (/ s -7))\n"
                 "        (set (at r a) (% s -7))))",
                 "t.f", {});
    CheckProgram(results,
                 "(let q (array i16 64)) (let r (array i16 64))\n"
                 "(fn f () void\n"
                 "    (for i u8 0 64\n"
                 "        (let s i16 (- (* (as i16 i) 1031) 30000))\n"
                 "        (set (at r i) (% s 7))\n"
                 "        (set (at q i) (/ s 7))))",
                 "t.f", {});
    CheckProgram(results,
                 "(let q (array u16 64)) (let r (array u16 64)) (let b u16 3)\n"
                 "(fn f () void\n"
                 "    (for i u8 0 64\n"
                 "        (let a u16 (* (as u16 i) 1031))\n"
                 "        (set (at q i) (/ a b))\n"
                 "        (set b (+ b 1))\n"
                 "        (set (at r i) (% a b))))",
                 "t.f", {});
}

// a pair divides once only while nothing in between can change the operands or the results
void TestPairEdges(TestResults& results) {
    // an operand set between the two
    CheckProgram(results,
                 "(let x u16 1000) (let q u16) (let r u16)\n"
                 "(fn f () void\n"
                 "    (set q (/ x 7))\n"
                 "    (set x (+ x 1))\n"
                 "    (set r (% x 7)))",
                 "t.f", {});
    // the operand is the destination of the first
    CheckProgram(results,
                 "(let x u16 1000) (let r u16)\n"
                 "(fn f () void\n"
                 "    (set x (/ x 7))\n"
                 "    (set r (% x 7)))",
                 "t.f", {});
    // through a pointer, with a store through another pointer to the same byte between
    CheckProgram(results,
                 "(let wb (array u8 8) (make (array u8 8) 200 201 202 203 204 205 206 207))\n"
                 "(let q u8) (let r u8)\n"
                 "(fn f () void\n"
                 "    (let p (ptr u8) (addr_of (at wb 3)))\n"
                 "    (let p2 (ptr u8) (addr_of (at wb 3)))\n"
                 "    (set q (/ (load p) 9))\n"
                 "    (store p2 50)\n"
                 "    (set r (% (load p) 9)))",
                 "t.f", {});
    // an array element, its index changed between, then the element itself
    CheckProgram(results,
                 "(let arr (array u8 8) (make (array u8 8) 10 20 30 40 50 60 70 80))\n"
                 "(let i u8 2) (let q u8) (let r u8) (let q2 u8) (let r2 u8)\n"
                 "(fn f () void\n"
                 "    (set q (/ (at arr i) 9))\n"
                 "    (set i 3)\n"
                 "    (set r (% (at arr i) 9))\n"
                 "    (set q2 (/ (at arr i) 9))\n"
                 "    (set (at arr 3) 77)\n"
                 "    (set r2 (% (at arr i) 9)))",
                 "t.f", {});
    // only one arm divides, or a loop that may not run: the join is another way in, with
    // rt_r holding the remainder of something else
    for (std::uint16_t c = 0; c < 2; c++) {
        CheckProgram(results,
                     "(let x u8 100) (let y u8 200) (let q u8) (let r u8)\n"
                     "(fn f ((c bool)) void\n"
                     "    (set q (% y 9))\n"
                     "    (if c (set q (/ x 7)) (set y 3))\n"
                     "    (set r (% x 7)))",
                     "t.f", {c});
        CheckProgram(results,
                     "(let x u8 100) (let y u8 200) (let q u8) (let r u8)\n"
                     "(fn f ((c bool)) void\n"
                     "    (if c (set q (/ x 7)) (set x 3))\n"
                     "    (set r (% x 7)))",
                     "t.f", {c});
        CheckProgram(results,
                     "(let x u16 1000) (let y u16 200) (let q u16) (let r u16)\n"
                     "(fn f ((n u8)) void\n"
                     "    (set q (% y 9))\n"
                     "    (for i u8 0 n (set q (/ x 7)))\n"
                     "    (set r (% x 7)))",
                     "t.f", {c});
    }
    // a call between: one that writes the operand, and one that divides other numbers
    std::string big = "(fn other ((a u16)) u16\n"
                      "    (let s u16 (/ a 5))\n"
                      "    (let t u16 (% a 3))\n"
                      "    (let u u16 (/ a 11))\n"
                      "    (+ (+ (+ s t) (+ u (% a 13))) (+ (/ a 17) (+ (% a 19) (/ a 23)))))\n";
    CheckProgram(results,
                 "(let g u16 5000) (let q u16) (let r u16) (let z u16)\n" + big +
                     "(fn bump () void (set g (+ g 3)))\n"
                     "(fn f () void\n"
                     "    (set q (/ g 7))\n"
                     "    (bump)\n"
                     "    (set r (% g 7))\n"
                     "    (set q (/ g 9))\n"
                     "    (set z (other 999))\n"
                     "    (set r (% g 9)))",
                 "t.f", {});
    // in one expression, nested, and an i/o read as the dividend (read twice, never reused)
    CheckProgram(results,
                 "(let x u16 12345) (let y u8)\n"
                 "(fn f () u16\n"
                 "    (set y (+ (% (io_read u8 0xC030) 7) (/ (io_read u8 0xC030) 7)))\n"
                 "    (+ (+ (/ x 7) (% x 7)) (/ (/ x 7) 7)))",
                 "t.f", {});
    // the loop counter as the dividend, in a register; a pair inside a loop body
    CheckProgram(results,
                 "(let q (array u8 200)) (let r (array u8 200)) (let x u8 0)\n"
                 "(fn f () void\n"
                 "    (for i u8 0 200\n"
                 "        (set (at q i) (/ i 7))\n"
                 "        (set x (+ x 1))\n"
                 "        (set (at r i) (+ (% i 7) (% x 7)))))",
                 "t.f", {});
    // a remainder used as a full u16, and a quotient into a struct field through a pointer
    CheckProgram(results,
                 "(struct S ((a u8) (w u16)))\n"
                 "(let s S) (let x u16 65000) (let y u16)\n"
                 "(fn f () void\n"
                 "    (let p (ptr S) (addr_of s))\n"
                 "    (store p w (/ x 10))\n"
                 "    (set y (+ (% x 10) 1000)))",
                 "t.f", {});
}

} // namespace

void RunBackendDivTests(TestResults& results) {
    TestByteConstants(results);
    TestWordConstants(results);
    TestRuntimePairs(results);
    TestPairEdges(results);
}

} // namespace tisple_tests
