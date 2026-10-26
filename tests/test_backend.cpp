// targeted differential tests: each program runs in the interpreter and on the 6502

#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

void TestScalars(TestResults& results) {
    CheckProgram(results,
                 "(let g u8 7) (let h u16 0x1234)\n"
                 "(fn f ((a u8) (b u16)) u16\n"
                 "    (set g (+ g a))\n"
                 "    (set h (- h b))\n"
                 "    (+ h (as u16 g)))",
                 "t.f", {200, 0x3456});
    CheckProgram(results,
                 "(fn f ((a i8) (b i8)) i16\n"
                 "    (let x i16 (as i16 a))\n"
                 "    (let y i16 (as i16 b))\n"
                 "    (- (* x y) (as i16 (as u8 a))))",
                 "t.f", {0xFE, 0x81});
    CheckProgram(results,
                 "(fn f ((a u16)) u8\n"
                 "    (as u8 (>> a 9)))",
                 "t.f", {0xFFFF});
}

void TestBranches(TestResults& results) {
    const char* text = "(let n u8 0)\n"
                       "(fn f ((a i8) (b i8) (c u16) (d u16)) u8\n"
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
                 "(fn f ((n u8)) u16\n"
                 "    (for i u8 0 n\n"
                 "        (for j u8 i n\n"
                 "            (if (= (& j 3) 1) (continue))\n"
                 "            (set sum (+ sum (as u16 (^ i j)))))\n"
                 "        (if (> i 20) (break)))\n"
                 "    (let k u16 0)\n"
                 "    (while (< k 300)\n"
                 "        (set k (+ k 7)))\n"
                 "    (+ sum k))",
                 "t.f", {40});
    CheckProgram(results,
                 "(let a (array u8 256))\n"
                 "(fn f () u8\n"
                 "    (for i u8 0 256 (set (at a i) (- 255 i)))\n"
                 "    (let s u8 0)\n"
                 "    (for i i8 -5 5 (set s (+ s (as u8 i))))\n"
                 "    (for i i16 -300 -290 (set s (+ s (as u8 i))))\n"
                 "    (for i u16 65530 65535 (set s (+ s (as u8 i))))\n"
                 "    (+ s (at a 3)))",
                 "t.f", {});
}

void TestArrays(TestResults& results) {
    CheckProgram(results,
                 "(struct P ((x u8) (y u16) (on bool)))\n"
                 "(let ps (array P 20))\n"
                 "(let ws (array u16 300))\n"
                 "(let packed (array u16 4))\n"
                 "(fn f ((n u8)) u16\n"
                 "    (let q (ptr u16) (addr_of (at packed 1)))\n"
                 "    (store q 0xBEEF)\n"
                 "    (for i u8 0 n\n"
                 "        (set (at ps i) (make P :x i :y (* (as u16 i) 300) :on (= (& i 1) 0)))\n"
                 "        (set (at ws (as u16 (* i 13))) (as u16 i)))\n"
                 "    (for i u16 0 300 (set (at ws i) (+ (at ws i) i)))\n"
                 "    (let t u16 0)\n"
                 "    (for i u8 0 n\n"
                 "        (if (at ps i on) (set t (+ t (at ps i y)))))\n"
                 "    (+ t (at ws 299)))",
                 "t.f", {20});
}

void TestPointers(TestResults& results) {
    CheckProgram(results,
                 "(struct V ((x i16) (y u8)))\n"
                 "(let vs (array V 4))\n"
                 "(let buf (array u8 64))\n"
                 "(fn fill ((p (ptr u8)) (n u8) (v u8)) void\n"
                 "    (for i u8 0 n (store (+ p (as u16 i)) (+ v i))))\n"
                 "(fn f () u16\n"
                 "    (fill (addr_of (at buf 3)) 50 9)\n"
                 "    (let p (ptr V) (addr_of (at vs 2)))\n"
                 "    (store p x -5)\n"
                 "    (store p y 77)\n"
                 "    (store (+ p 1) (make V :x 300 :y (load p y)))\n"
                 "    (let b (ptr u8) (as (ptr u8) (+ (as u16 (addr_of buf)) 10)))\n"
                 "    (+ (as u16 (load p x)) (+ (as u16 (load b)) (as u16 (load (+ p 1) x)))))",
                 "t.f", {});
}

void TestCalls(TestResults& results) {
    // siblings share frame bytes: g's and h's frames overlap, f's must not
    CheckProgram(results,
                 "(fn g ((a u8) (b u8)) u8 (let t u8 (* a b)) (+ t 1))\n"
                 "(fn h ((a u16)) u16 (let t u16 (+ a a)) (- t 3))\n"
                 "(fn k ((a u8) (b u16) (c u8)) u16\n"
                 "    (+ (as u16 (g a c)) (h b)))\n"
                 "(fn f ((x u8)) u16\n"
                 "    (+ (k (g x 3) (h (as u16 x)) (g 2 x)) (k x (h 7) (g x x))))",
                 "t.f", {13});
}

// small functions are inlined: returns in the middle, returns whose value inlines more
// calls (this crashed once), loops around inlined calls
void TestInline(TestResults& results) {
    CheckProgram(results,
                 "(let g u8 0)\n"
                 "(let out (array u8 16))\n"
                 "(fn inc ((x u16)) u16 (+ x 1))\n"
                 "(fn twice ((x u16)) u16\n"
                 "    (if (= x 0) (return (inc (inc (inc x)))))\n"
                 "    (inc (inc x)))\n"
                 "(fn put ((i u8) (v u8)) void (set (at out (& i 15)) v) (set g (+ g v)))\n"
                 "(fn small ((x u8)) u8 (if (> x 9) (return 9)) x)\n"
                 "(fn f ((n u16)) u16\n"
                 "    (for i u8 0 16 (put i (small i)))\n"
                 "    (+ (twice n) (+ (twice 0) (as u16 g))))",
                 "t.f", {1000});
}

void TestMatch(TestResults& results) {
    const char* text =
        "(enum E (a b c d e f g))\n"
        "(fn dense ((x u8)) u8\n"
        "    (match x (3 10) (4 11) (5 12) (6 13) (7 14) (9 15) (_ 99)))\n"
        "(fn sparse ((x u16)) u16\n"
        "    (match x (0 1) (1000 2) (0xFFFF 3) (_ 4)))\n"
        "(fn via_enum ((x u8)) u8\n"
        "    (let e E (match x (0 E.a) (1 E.b) (2 E.c) (3 E.d) (4 E.e) (5 E.f) (_ E.g)))\n"
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
                 "(fn f () u16\n"
                 "    (io_read u8 0xC030)\n"
                 "    (io_write u8 0xC010 (io_read u8 0xC000))\n"
                 "    (io_write u16 addr 0x1234)\n"
                 "    (let a u16 (io_read u16 (+ addr 2)))\n"
                 "    (+ a (as u16 (io_read u8 addr))))",
                 "t.f", {});
}

// bugs the fuzzer found, smallest form
void TestRegressions(TestResults& results) {
    // fuzz seed 7177: a copy between two elements of one split field was dropped, because
    // source and destination differed only in their index
    CheckProgram(results,
                 "(struct S ((a u8) (b i16)))\n"
                 "(let sa (array S 8))\n"
                 "(let g u8 3)\n"
                 "(fn f () void\n"
                 "    (set (at sa 5 b) 1234)\n"
                 "    (set (at sa 2 a) 5)\n"
                 "    (set (at sa (& g 7) b) (at sa (& (at sa (& g 6) a) 7) b)))",
                 "t.f", {});
    // fuzz seeds 504457 and 572343: (<< g 8) reads g's low byte as its high byte, so
    // writing g low byte first clobbered it
    CheckProgram(results,
                 "(let g u16 0x1234)\n"
                 "(let h u16 0x5678)\n"
                 "(let k (array u16 4))\n"
                 "(let p (ptr u16))\n"
                 "(fn f () u16\n"
                 "    (set (at k 1) 93)\n"
                 "    (set g (+ (<< g 8) (at k 1)))\n"
                 "    (set h (~ (<< h 8)))\n"
                 "    (set p (addr_of h))\n"
                 "    (store p (| (<< (load p) 8) (>> (load p) 8)))\n"
                 "    (set g (<< g 8))\n"
                 "    (+ g h))",
                 "t.f", {});
    // fuzz seed 15082: a discarded trial of an inner loop made a derived pointer, the
    // retry handed its zero-page slot to a second one
    CheckProgram(results,
                 "(struct S ((a u8) (b i16)))\n"
                 "(let g0 u8 12)\n"
                 "(let wb (array u8 1024))\n"
                 "(let sa (array S 8))\n"
                 "(fn f () u8\n"
                 "    (for i u8 0 8 (set (at sa i a) (* i 3)))\n"
                 "    (let wp (ptr u8) (addr_of (at wb (as u16 (& (~ g0) 15)))))\n"
                 "    (for n u16 250 262\n"
                 "        (for k u8 0 3\n"
                 "            (set g0 (~ g0))\n"
                 "            (store (+ wp n) (at sa (& k 7) a)))\n"
                 "        (set (at wb n) g0)\n"
                 "        (for k u8 3 5 (store (+ wp n) k)))\n"
                 "    (at wb 251))",
                 "t.f", {});
    // fuzz seeds 2000046127 and 9000008810: a 16-bit element indexed by the low byte of its
    // own destination; writing the low byte first changed the index the high byte read
    // with (a plain set, an add, a store through a pointer to the index)
    CheckProgram(results,
                 "(let g3 i16 32767)\n"
                 "(let g4 u16 0x0102)\n"
                 "(let w u16 5)\n"
                 "(let arr (array i16 256))\n"
                 "(let ws (array u16 256))\n"
                 "(fn f () void\n"
                 "    (for i u8 0 255\n"
                 "        (set (at arr i) (* (as i16 i) 300))\n"
                 "        (set (at ws i) (* (as u16 i) 517)))\n"
                 "    (set (at arr 255) 300)\n"
                 "    (set g3 (at arr (as u8 g3)))\n"
                 "    (set g4 (+ (at ws (as u8 g4)) 1))\n"
                 "    (let pw (ptr u16) (addr_of w))\n"
                 "    (store pw (at ws (as u8 w))))",
                 "t.f", {});
    // fuzz seed 7000005809: a let read once by an index let was forwarded, so the index
    // read its value after a later set; the same through two forwarded lets
    CheckProgram(results,
                 "(let g0 u8 12)\n"
                 "(let h u8 0)\n"
                 "(let wb (array u8 64))\n"
                 "(fn f () void\n"
                 "    (let p (ptr u8) (addr_of (at wb 1)))\n"
                 "    (let ix39 u8 g0)\n"
                 "    (let ix40 u8 (+ ix39 1))\n"
                 "    (set g0 40)\n"
                 "    (store (+ p (as u16 (- ix40 1))) 187)\n"
                 "    (let a u8 (* g0 5))\n"
                 "    (let b u8 (* a 3))\n"
                 "    (set g0 5)\n"
                 "    (set h b))",
                 "t.f", {});
    // fuzz seeds 3002869864, 7003579163, 1003065560 and 5 more: a 16-bit shift by 5 to 7
    // writes its result's high byte before it reads the source's; the source was an element
    // of the same array by a variable index, or the same bytes through a pointer, and was
    // not seen to overlap
    CheckProgram(results,
                 "(let gs u8 2)\n"
                 "(let w u16 0x1234)\n"
                 "(let arr (array u16 4) (make (array u16 4) 13487 60195 47343 28371))\n"
                 "(fn f () u16\n"
                 "    (set (at arr 2) (<< (at arr (& gs 3)) 5))\n"
                 "    (set (at arr 1) (>> (at arr (& (+ gs 255) 3)) 6))\n"
                 "    (let p (ptr u16) (addr_of w))\n"
                 "    (set w (<< (load p) 6))\n"
                 "    (+ w (at arr 2)))",
                 "t.f", {});
    // u16 loops in every shape: page crossing, ending on a page, through the top
    CheckProgram(results,
                 "(let wb (array u8 1024))\n"
                 "(let ww (array u16 600))\n"
                 "(let total u16 0)\n"
                 "(fn f ((n u16)) u16\n"
                 "    (let p (ptr u8) (addr_of (at wb 7)))\n"
                 "    (for i u16 250 n (set (at ww i) (+ i 1)) (store (+ p i) (as u8 i)))\n"
                 "    (for i u16 500 512 (set (at wb i) (as u8 (>> i 1))))\n"
                 "    (for i u16 65530 65536 (set total (+ total i)))\n"
                 "    (for i u16 0 600 (set total (+ total (at ww i))))\n"
                 "    (+ total (as u16 (at wb 300))))",
                 "t.f", {590});
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
    TestInline(results);
    TestMatch(results);
    TestIo(results);
}

} // namespace tisple_tests
