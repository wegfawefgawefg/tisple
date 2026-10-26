// targeted differential tests: each program runs in the interpreter and on the 6502

#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

void TestScalars(TestResults& results) {
    CheckProgram(results,
                 "(let g u8 7) (let h u16 0x1234)\n"
                 "(fn f (args (a u8) (b u16)) (returns u16)\n"
                 "    (set g (+ g a))\n"
                 "    (set h (- h b))\n"
                 "    (+ h (u16 g)))",
                 "t.f", {200, 0x3456});
    CheckProgram(results,
                 "(fn f (args (a i8) (b i8)) (returns i16)\n"
                 "    (let x (i16 a))\n"
                 "    (let y (i16 b))\n"
                 "    (- (* x y) (i16 (u8 a))))",
                 "t.f", {0xFE, 0x81});
    CheckProgram(results,
                 "(fn f (args (a u16)) (returns u8)\n"
                 "    (u8 (>> a 9)))",
                 "t.f", {0xFFFF});
}

void TestBranches(TestResults& results) {
    const char* text = "(let n u8 0)\n"
                       "(fn f (args (a i8) (b i8) (c u16) (d u16)) (returns u8)\n"
                       "    (set n 0)\n"
                       "    (if (< a b) (set n (| n 1)))\n"
                       "    (if (> a b) (set n (| n 2)))\n"
                       "    (if (<= c d) (set n (| n 4)))\n"
                       "    (if (and (>= c d) (!= a 0)) (set n (| n 8)))\n"
                       "    (if (or (= c 0) (not (= b 5))) (set n (| n 16)))\n"
                       "    (if (> c 1000) (set n (| n 32)))\n"
                       "    (if (< a 0) (set n (| n 64)))\n"
                       "    n)";
    Compiled c = CompileText(text);
    Diff d;
    CHECK(results, StartDiff(d, c));
    std::uint16_t values[] = {0, 1, 5, 0x7F, 0x80, 0xFF, 0xFE};
    std::uint16_t wide[] = {0, 1, 999, 1000, 1001, 0x8000, 0xFFFF};
    for (std::uint16_t a : values) {
        for (std::uint16_t b : values) {
            CheckDiff(results, d, "t.f", {a, b, wide[a % 7], wide[b % 7]});
        }
    }
}

void TestLoops(TestResults& results) {
    CheckProgram(results,
                 "(let sum u16 0)\n"
                 "(fn f (args (n u8)) (returns u16)\n"
                 "    (for i 0 n\n"
                 "        (for j i n\n"
                 "            (if (= (& j 3) 1) (continue))\n"
                 "            (set sum (+ sum (u16 (^ i j)))))\n"
                 "        (if (> i 20) (break)))\n"
                 "    (let k u16 0)\n"
                 "    (while (< k 300)\n"
                 "        (set k (+ k 7)))\n"
                 "    (+ sum k))",
                 "t.f", {40});
    CheckProgram(results,
                 "(let a (array u8 256))\n"
                 "(fn f (returns u8)\n"
                 "    (for i u8 0 256 (set (at a i) (- 255 i)))\n"
                 "    (let s u8 0)\n"
                 "    (for i i8 -5 5 (set s (+ s (u8 i))))\n"
                 "    (for i i16 -300 -290 (set s (+ s (u8 i))))\n"
                 "    (for i u16 65530 65535 (set s (+ s (u8 i))))\n"
                 "    (+ s (at a 3)))",
                 "t.f", {});
}

void TestArrays(TestResults& results) {
    CheckProgram(results,
                 "(struct P (x u8) (y u16) (on bool))\n"
                 "(let ps (array P 20))\n"
                 "(let ws (array u16 300))\n"
                 "(let packed (array u16 4))\n"
                 "(fn f (args (n u8)) (returns u16)\n"
                 "    (let q (addr_of (at packed 1)))\n"
                 "    (store q 0xBEEF)\n"
                 "    (for i u8 0 n\n"
                 "        (set (at ps i) (P :x i :y (* (u16 i) 300) :on (= (& i 1) 0)))\n"
                 "        (set (at ws (u16 (* i 13))) (u16 i)))\n"
                 "    (for i u16 0 300 (set (at ws i) (+ (at ws i) i)))\n"
                 "    (let t u16 0)\n"
                 "    (for i u8 0 n\n"
                 "        (if (. (at ps i) on) (set t (+ t (. (at ps i) y)))))\n"
                 "    (+ t (at ws 299)))",
                 "t.f", {20});
}

void TestPointers(TestResults& results) {
    CheckProgram(results,
                 "(struct V (x i16) (y u8))\n"
                 "(let vs (array V 4))\n"
                 "(let buf (array u8 64))\n"
                 "(fn fill (args (p (ptr u8)) (n u8) (v u8))\n"
                 "    (for i 0 n (store (+ p (u16 i)) (+ v i))))\n"
                 "(fn f (returns u16)\n"
                 "    (fill (addr_of (at buf 3)) 50 9)\n"
                 "    (let p (addr_of (at vs 2)))\n"
                 "    (store p x -5)\n"
                 "    (store p y 77)\n"
                 "    (store (+ p 1) (V :x 300 :y (load p y)))\n"
                 "    (let b (as (ptr u8) (+ (as u16 (addr_of buf)) 10)))\n"
                 "    (+ (u16 (load p x)) (+ (u16 (load b)) (u16 (load (+ p 1) x)))))",
                 "t.f", {});
}

void TestCalls(TestResults& results) {
    // siblings share frame bytes: g's and h's frames overlap, f's must not
    CheckProgram(results,
                 "(fn g (args (a u8) (b u8)) (returns u8) (let t (* a b)) (+ t 1))\n"
                 "(fn h (args (a u16)) (returns u16) (let t (+ a a)) (- t 3))\n"
                 "(fn k (args (a u8) (b u16) (c u8)) (returns u16)\n"
                 "    (+ (u16 (g a c)) (h b)))\n"
                 "(fn f (args (x u8)) (returns u16)\n"
                 "    (+ (k (g x 3) (h (u16 x)) (g 2 x)) (k x (h 7) (g x x))))",
                 "t.f", {13});
}

void TestMatch(TestResults& results) {
    const char* text =
        "(enum E a b c d e f g)\n"
        "(fn dense (args (x u8)) (returns u8)\n"
        "    (match x (3 10) (4 11) (5 12) (6 13) (7 14) (9 15) (_ 99)))\n"
        "(fn sparse (args (x u16)) (returns u16)\n"
        "    (match x (0 1) (1000 2) (0xFFFF 3) (_ 4)))\n"
        "(fn via_enum (args (x u8)) (returns u8)\n"
        "    (let e (match x (0 E.a) (1 E.b) (2 E.c) (3 E.d) (4 E.e) (5 E.f) (_ E.g)))\n"
        "    (match e (E.a 1) (E.b 2) (E.c 3) (E.d 4) (E.e 5) (E.f 6) (E.g 7)))";
    Compiled c = CompileText(text);
    Diff d;
    bool started = StartDiff(d, c);
    if (!started) {
        std::fprintf(stderr, "match: %s\n", d.message.c_str());
        return;
    }
    for (std::uint16_t x = 0; x < 260; x++) {
        CheckDiff(results, d, "t.dense", {static_cast<std::uint16_t>(x & 0xFF)});
        CheckDiff(results, d, "t.via_enum", {static_cast<std::uint16_t>(x & 0xFF)});
    }
    for (std::uint16_t x :
         {std::uint16_t(0), std::uint16_t(1), std::uint16_t(999), std::uint16_t(1000),
          std::uint16_t(1001), std::uint16_t(0xFFFE), std::uint16_t(0xFFFF)}) {
        CheckDiff(results, d, "t.sparse", {x});
    }
}

void TestIo(TestResults& results) {
    CheckProgram(results,
                 "(let addr u16 0xC030)\n"
                 "(fn f (returns u16)\n"
                 "    (io_read u8 0xC030)\n"
                 "    (io_write u8 0xC010 (io_read u8 0xC000))\n"
                 "    (io_write u16 addr 0x1234)\n"
                 "    (let a (io_read u16 (+ addr 2)))\n"
                 "    (+ a (u16 (io_read u8 addr))))",
                 "t.f", {});
}

// bugs the fuzzer found, smallest form
void TestRegressions(TestResults& results) {
    // fuzz seed 7177: a copy between two elements of one split field was dropped, because
    // source and destination differed only in their index
    CheckProgram(results,
                 "(struct S (a u8) (b i16))\n"
                 "(let sa (array S 8))\n"
                 "(let g u8 3)\n"
                 "(fn f\n"
                 "    (set (. (at sa 5) b) 1234)\n"
                 "    (set (. (at sa 2) a) 5)\n"
                 "    (set (. (at sa (& g 7)) b) (. (at sa (& (. (at sa (& g 6)) a) 7)) b)))",
                 "t.f", {});
}

} // namespace

void RunBackendTests(TestResults& results) {
    TestRegressions(results);
    TestScalars(results);
    TestBranches(results);
    TestLoops(results);
    TestArrays(results);
    TestPointers(results);
    TestCalls(results);
    TestMatch(results);
    TestIo(results);
}

} // namespace tisple_tests
