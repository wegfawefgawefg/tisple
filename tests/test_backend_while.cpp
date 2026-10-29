// while loops with variables in registers (gen_while.cpp), facts from conditions
// (gen_facts.cpp), step pointers for stepped u16 variables (gen_step.cpp), and u16 for loops
// with reduced lets; on the 6502 against the interpreter, at the edges: pages crossed in both
// directions, wraps, early exits, sets in arms, values used after the loop, calls

#include "test_backend.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace tisple_tests {

namespace {

// a string buffer over three pages, a word buffer, a struct array, and setup that fills
// them; s holds runs of nonzero bytes ended by zeros at known places
const char* const kData =
    "(struct P ((a u8) (b u16) (c u8)))\n"
    "(let g u8 0)\n"
    "(let gw u16 0)\n"
    "(let s (array u8 768))\n"
    "(let t (array u8 1024))\n"
    "(let bt (array u8 256))\n"
    "(let w (array u16 300))\n"
    "(let ps (array P 80))\n"
    "(let small (array u8 16))\n"
    "(let gp (ptr u8))\n"
    "(fn bump () void (set g (+ g 1)))\n"
    // too big to inline: a JSR
    "(fn big () void (set g (+ g 1)) (set g (^ g 3)) (set g (+ g (at small 1)))\n"
    "    (set g (+ g (at small 2))) (set g (+ g (at small 3))) (set g (+ g (at small 4)))\n"
    "    (set g (+ g (at small 5))) (set g (+ g (at small 6))))\n"
    "(fn setup () void\n"
    "    (for i u16 0 768 (set (at s i) (as u8 (+ (& i 0x7F) 1))))\n"
    "    (set (at s 0) 0) (set (at s 3) 0) (set (at s 255) 0) (set (at s 256) 0)\n"
    "    (set (at s 300) 0) (set (at s 511) 0) (set (at s 600) 0) (set (at s 767) 0)\n"
    "    (for i u16 0 1024 (set (at t i) (as u8 (* i 7))))\n"
    "    (for i u8 0 255 (set (at bt i) (+ (* i 5) 1)))\n"
    "    (set (at bt 3) 0) (set (at bt 200) 0) (set (at bt 255) 77)\n"
    "    (for i u16 0 300 (set (at w i) (* i 77)))\n"
    "    (for i u8 0 80 (set (at ps i a) i) (set (at ps i b) (* (as u16 i) 300)))\n"
    "    (for i u8 0 16 (set (at small i) (* i 13)))\n"
    "    (set gp (addr_of (at t 5))))\n";

// setup, then f for every argument in args (one or two of them)
void CheckCalls(TestResults& results, const std::string& body,
                const std::vector<std::vector<std::uint16_t>>& args) {
    std::string text = std::string(kData) + body;
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c) && DiffCall(d, "t.setup", {});
    for (std::size_t i = 0; i < args.size() && ok; i++) {
        ok = DiffCall(d, "t.f", args[i]);
        if (!ok) {
            std::fprintf(stderr, "f(");
            for (std::uint16_t a : args[i]) {
                std::fprintf(stderr, "%u ", a);
            }
            std::fprintf(stderr, "): ");
        }
    }
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

std::vector<std::vector<std::uint16_t>> Starts() {
    std::vector<std::vector<std::uint16_t>> out;
    for (int v : {0,   1,   2,   3,   4,   5,   100, 250, 254, 255, 256, 257, 299,
                  300, 301, 450, 510, 511, 512, 598, 599, 600, 601, 700, 766, 767}) {
        out.push_back({static_cast<std::uint16_t>(v)});
    }
    return out;
}

std::vector<std::vector<std::uint16_t>> Bytes() {
    std::vector<std::vector<std::uint16_t>> out;
    for (std::uint16_t v = 0; v < 256; v += 3) {
        out.push_back({v, static_cast<std::uint16_t>((v * 37 + 11) & 0xFF)});
    }
    out.push_back({255, 0});
    out.push_back({254, 255});
    return out;
}

// a u16 variable stepped by the loop: low byte in Y, high byte in memory, pointers per base
void TestWideWhile(TestResults& results) {
    const char* bodies[] = {
        // strlen: the step last, the test at the top, two tests a time; t's zeros start
        // pages, so its scans cross one, from odd and even starts
        "(fn f ((n u16)) u16 (while (!= (at s n) 0) (set n (+ n 1))) n)",
        "(fn f ((n u16)) u16 (while (!= (at t n) 0) (set n (+ n 1))) n)",
        "(fn f ((n u16)) u16 (while (!= (at t (+ n 1)) 0) (set n (+ n 1))) n)",
        // through a pointer variable, and a pointer plus an offset
        "(fn f ((n u16)) u16 (let p (ptr u8) (addr_of (at s 0)))\n"
        "    (while (!= (load (+ p n)) 0) (set n (+ n 1))) n)",
        "(fn f ((n u16)) u16 (let p (ptr u8) (addr_of (at s 0)))\n"
        "    (while (and (< n 700) (!= (load (+ (+ p 3) n)) 0)) (set n (+ n 1))) n)",
        // counting down across pages, to zero
        "(fn f ((n u16)) u16 (while (and (!= n 0) (!= (at s n) 0)) (set n (- n 1))) n)",
        "(fn f ((n u16)) u16 (while (!= n 0) (set n (- n 1)) (set g (+ g (at t n)))) n)",
        // steps by constants below and above a page, both ways
        "(fn f ((n u16)) u16 (while (< n 760) (set (at s n) (+ (at s n) 1)) (set n (+ n 3))) n)",
        "(fn f ((n u16)) u16 (while (< n 1000) (set g (^ g (at t n))) (set n (+ n 255))) n)",
        "(fn f ((n u16)) u16 (while (< n 1000) (set g (^ g (at t n))) (set n (+ n 256))) n)",
        "(fn f ((n u16)) u16 (while (< n 1000) (set g (^ g (at t n))) (set n (+ n 300))) n)",
        "(fn f ((n u16)) u16 (while (> n 6) (set g (+ g (at t n))) (set n (- n 7))) n)",
        "(fn f ((n u16)) u16 (while (< n 1000) (set g (+ g (at t n))) (set n (- n 300))) n)",
        // by a variable the loop never changes (the sieve), and by one it does
        "(fn f ((n u16)) u16 (let k u16 (+ n n)) (let i u16 (+ n 2))\n"
        "    (while (< k 1000) (set (at t k) (+ (at t k) 1)) (set k (+ k i))) k)",
        "(fn f ((n u16)) u16 (let k u16 0) (let i u16 1)\n"
        "    (while (< k 1000) (set g (+ g (at t k))) (set k (+ k i)) (set i (+ i n))) k)",
        // any other value: each pointer's high byte worked out again
        "(fn f ((n u16)) u16 (let k u16 (& n 511)) (let c u8 0)\n"
        "    (while (< c 40) (set g (+ g (at t k))) (set k (& (+ (* k 5) 3) 1023))\n"
        "        (set c (+ c 1))) k)",
        "(fn f ((n u16)) u16 (let k u16 (& n 255)) (let c u8 0)\n"
        "    (while (< c 9) (set g (+ g (at t k))) (set k (at w (+ (& k 127) 1)))\n"
        "        (set k (& k 1023)) (set c (+ c 1))) k)",
        // the step in an arm, the step not last, reads after it, break and continue
        "(fn f ((n u16)) u16 (let c u8 0)\n"
        "    (while (< c 200) (if (= (& (at t n) 1) 0) (set n (+ n 1)))\n"
        "        (set g (+ g (at t n))) (set c (+ c 1))) n)",
        "(fn f ((n u16)) u16\n"
        "    (while (< n 1000) (set n (+ n 1)) (if (= (at t n) 0) (break))\n"
        "        (if (= (& (at t n) 3) 1) (continue)) (set g (+ g (at t n)))) n)",
        // the value after the loop, used in a sum; and a loop that never runs
        "(fn f ((n u16)) u16 (let m u16 n) (while (> m 1000) (set m (+ m 1)))\n"
        "    (while (!= (at s m) 0) (set m (+ m 1))) (+ m (* n 3)))",
        // a call that is not inlined: no registers
        "(fn f ((n u16)) u16 (while (!= (at s n) 0) (big) (bump) (set n (+ n 1))) n)",
        // a u16 array (split bytes) and a struct array indexed by it
        "(fn f ((n u16)) u16 (let k u16 (& n 255)) (let sum u16 0)\n"
        "    (while (< k 299) (set sum (+ sum (at w k))) (set (at w k) (+ (at w k) 1))\n"
        "        (set k (+ k 1))) sum)",
        "(fn f ((n u16)) u16 (let k u16 (& n 63)) (let sum u16 0)\n"
        "    (while (< k 80) (set sum (+ sum (at ps (as u8 k) b))) (set k (+ k 1))) sum)",
        // nested loops in the body, which then cannot have Y
        "(fn f ((n u16)) u16 (let k u16 (& n 511))\n"
        "    (while (< k 700) (for j u8 0 3 (set g (+ g (at small j)))) (set (at s k) g)\n"
        "        (set k (+ k 50))) k)",
    };
    for (const char* body : bodies) {
        CheckCalls(results, body, Starts());
    }
    // strlen's loop: no 16-bit add inside it, two steps of Y a time
    Compiled c = CompileText(std::string(kData) +
                             "(fn f ((n u16)) u16 (while (!= (at s n) 0) (set n (+ n 1))) n)");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::INY) == 2);
    CHECK(results, CountMn(c, "t.f", tisple::Mn::ADC) == 1);
}

// byte variables in X and Y: steps, other sets, values after the loop, returns, calls
void TestByteWhile(TestResults& results) {
    const char* bodies[] = {
        // the sort: j stepped down, v read only, a facts-folded j - 1
        "(fn f ((x u8) (y u8)) u8 (let v u8 y) (let j u8 x)\n"
        "    (while (and (!= j 0) (> (at bt (- j 1)) v)) (set (at bt j) (at bt (- j 1)))\n"
        "        (set j (- j 1)))\n"
        "    (set (at bt j) v) j)",
        // the search: j counted up and compared after the loop
        "(fn f ((x u8) (y u8)) u8 (let p (ptr u8) (addr_of (at t (as u16 x)))) (let j u8 0)\n"
        "    (while (and (< j 6) (= (load (+ p j)) (at small j))) (set j (+ j 1)))\n"
        "    (if (= j 6) 1 (+ j 10)))",
        // two variables, both set, one by a computed value
        "(fn f ((x u8) (y u8)) u8 (let a u8 x) (let b u8 y) (let c u8 0)\n"
        "    (while (and (!= a b) (< c 50)) (set a (+ a 1)) (set b (- (at bt b) 3))\n"
        "        (set c (+ c 1)) (set g (+ g a))) (+ a b))",
        "(fn f ((x u8) (y u8)) u8 (let a u8 x) (let b u8 0)\n"
        "    (while (> a 3) (set b (+ b (at small (& a 15)))) (set a (>> a 1))) (^ a b))",
        // i8 variables, a signed compare
        "(fn f ((x u8) (y u8)) u8 (let a i8 (as i8 x))\n"
        "    (while (< a 50) (set a (+ a 7)) (set g (+ g (as u8 a)))) (as u8 a))",
        // return from inside the loop, break, continue
        "(fn f ((x u8) (y u8)) u8 (let a u8 x)\n"
        "    (while (!= a y) (set a (+ a 1)) (if (= (at bt a) 0) (return 99))) a)",
        "(fn f ((x u8) (y u8)) u8 (let a u8 x) (let n u8 0)\n"
        "    (while true (set a (+ a 3)) (set n (+ n 1)) (if (> n 40) (break))\n"
        "        (if (= (& a 1) 0) (continue)) (set g (+ g a))) (+ a n))",
        // inlined calls reading the variable and taking it as an argument
        "(fn twice ((v u8)) u8 (+ v v))\n"
        "(fn f ((x u8) (y u8)) u8 (let a u8 x)\n"
        "    (while (< a 200) (set g (+ g (twice a))) (set a (+ a 9))) a)",
        // a call that is not inlined: no registers
        "(fn f ((x u8) (y u8)) u8 (let a u8 x)\n"
        "    (while (< a 250) (big) (set a (+ a 1))) a)",
        // nested: a for loop inside, a while inside a for
        "(fn f ((x u8) (y u8)) u8 (let a u8 0)\n"
        "    (while (< a x) (for k u8 0 4 (set g (+ g (at small k)))) (set a (+ a 1))) a)",
        "(fn f ((x u8) (y u8)) u8 (let n u8 0)\n"
        "    (for i u8 0 (& x 15) (let a u8 i) (while (!= a 0) (set n (+ n a)) (set a (- a 1))))\n"
        "    n)",
        // a set inside an arm and a 16-bit value read with an index in a register
        "(fn f ((x u8) (y u8)) u16 (let a u8 x) (let sum u16 0)\n"
        "    (while (< a 250) (set sum (+ sum (at w (as u16 a)))) (if (> (at bt a) 100) (set a (+ "
        "a 2)))\n"
        "        (set a (+ a 1))) sum)",
        // the outer for counter's register taken by the while: the counter read inside, after,
        // and by the for's own step (the sort, with the array the outer loop walks)
        "(fn f ((x u8) (y u8)) u8\n"
        "    (for i u8 1 (+ (& x 63) 2) (let v u8 (at bt i)) (let j u8 i)\n"
        "        (while (and (!= j 0) (> (at bt (- j 1)) v)) (set (at bt j) (at bt (- j 1)))\n"
        "            (set g (+ g i)) (set j (- j 1)))\n"
        "        (set (at bt j) v) (set g (^ g (+ i j))))\n"
        "    g)",
        "(fn f ((x u8) (y u8)) u8\n"
        "    (for i u8 0 (& y 31) (let a u8 x) (let b u8 i)\n"
        "        (while (and (!= a b) (< a 250)) (set a (+ a 1)) (set b (- b 1)) (set g (+ g i)))\n"
        "        (set g (+ g (+ a (at bt i)))))\n"
        "    g)",
        // two for counters in X and Y, a while inside both taking them
        "(fn f ((x u8) (y u8)) u8\n"
        "    (for i u8 0 (& x 7) (for k u8 0 (& y 7) (let a u8 (+ i k)) (let b u8 x)\n"
        "        (while (> a 0) (set b (+ b (at bt (+ a k)))) (set a (- a 1)))\n"
        "        (set g (^ g (+ b (+ i k))))))\n"
        "    g)",
        // a while with a return in it keeps its hands off the counter's register
        "(fn f ((x u8) (y u8)) u8\n"
        "    (for i u8 0 (& x 15) (let a u8 0) (let b u8 y)\n"
        "        (while (< a 5) (set a (+ a 1)) (set b (+ b i)) (if (= b 77) (return i)))\n"
        "        (set g (+ g b)))\n"
        "    g)",
    };
    for (const char* body : bodies) {
        CheckCalls(results, body, Bytes());
    }
}

// facts: offsets folded into addresses only where the condition keeps them in range
void TestFacts(TestResults& results) {
    const char* bodies[] = {
        "(fn f ((x u8) (y u8)) u8 (if (and (!= x 0) (> (at bt (- x 1)) 50)) 1 0))",
        "(fn f ((x u8) (y u8)) u8 (if (!= x 0) (at bt (- x 1)) (at bt (+ x 1))))",
        "(fn f ((x u8) (y u8)) u8 (if (= x 0) (at bt 7) (at bt (- x 1))))",
        "(fn f ((x u8) (y u8)) u8 (if (> x 3) (at bt (- x 4)) (at bt (+ x 200))))",
        "(fn f ((x u8) (y u8)) u8 (if (< x 250) (at bt (+ x 5)) 0))",
        "(fn f ((x u8) (y u8)) u8 (if (or (= x 0) (= (at bt (- x 1)) 3)) 1 2))",
        "(fn f ((x u8) (y u8)) u8 (if (not (= x 0)) (at bt (- x 1)) 9))",
        // a set in the arm ends the fact, before and after the read
        "(fn f ((x u8) (y u8)) u8 (let a u8 x)\n"
        "    (if (!= a 0) (do (set g (at bt (- a 1))) (set a (- a 1)) (set g (+ g (at bt (- a "
        "1)))))) g)",
        "(fn f ((x u8) (y u8)) u8 (let a u8 x)\n"
        "    (if (!= a 0) (do (set a y) (set g (at bt (- a 1))))) g)",
        // a while body: the fact holds until the step
        "(fn f ((x u8) (y u8)) u8 (let a u8 x) (let n u8 0)\n"
        "    (while (!= a 0) (set n (+ n (at bt (- a 1)))) (set a (- a 1))\n"
        "        (set n (^ n (at bt (- a 1))))) n)",
        // a loop in the arm that changes the variable
        "(fn f ((x u8) (y u8)) u8 (let a u8 x)\n"
        "    (if (!= a 0) (for k u8 0 3 (set g (+ g (at bt (- a 1)))) (set a (- a 1)))) g)",
        // through a pointer, and a fact on a loop counter
        "(fn f ((x u8) (y u8)) u8 (let p (ptr u8) (addr_of (at t 300)))\n"
        "    (if (> x 1) (load (+ p (- x 2))) (load (+ p x))))",
        "(fn f ((x u8) (y u8)) u8 (let n u8 0)\n"
        "    (for i u8 0 x (if (!= i 0) (set n (+ n (at bt (- i 1)))))) n)",
    };
    for (const char* body : bodies) {
        CheckCalls(results, body, Bytes());
    }
}

// step pointers: u16 for counters in memory and u16 locals a loop steps
// the race's search kernel, whose u16 counter is replaced by its text pointer, with the
// needle at every place near the start and the end, and nowhere
void TestReplacedCounter(TestResults& results) {
    const char* text =
        "(let text (array u8 4096) :address 0x2000)\n"
        "(let needle (array u8 6) :address 0x5E00)\n"
        "(let result u16 :address 0x5F00)\n"
        "(fn setup ((where u16)) void\n"
        "    (for i u16 0 4096 (set (at text i) (as u8 (& i 0x3F))))\n"
        "    (for k u8 0 6 (set (at needle k) (+ k 0x40))\n"
        "        (if (< where 4096) (set (at text (& (+ where (as u16 k)) 4095)) (+ k "
        "0x40)))))\n"
        "(fn main () void\n"
        "    (for i u16 0 4091 (let p (ptr u8) (addr_of (at text i))) (let j u8 0)\n"
        "        (while (and (< j 6) (= (load (+ p j)) (at needle j))) (set j (+ j "
        "1)))\n"
        "        (if (= j 6) (do (set result i) (return))))\n"
        "    (set result 0xFFFF))\n";
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c);
    for (int at : {0, 1, 2, 255, 256, 257, 1000, 3500, 4089, 4090, 4091, 5000}) {
        ok = ok && DiffCall(d, "t.setup", {static_cast<std::uint16_t>(at)}) &&
             DiffCall(d, "t.main", {});
    }
    if (!ok) {
        std::fprintf(stderr, "replaced counter:\n%s\n  %s\n", text, d.message.c_str());
    }
    CHECK(results, ok);
    // the counter is gone from the loop: no INC of it
    CHECK(results, CountMn(c, "t.main", tisple::Mn::INC) == 2);
}

void TestStepPointers(TestResults& results) {
    const char* bodies[] = {
        // the sieve's outer loop: a while inside takes Y, the counter stays in memory
        "(fn f ((n u16)) u16 (let count u16 0)\n"
        "    (for i u16 (& n 511) 700\n"
        "        (if (!= (& (at t i) 3) 0) (do (set count (+ count 1)) (let k u16 i)\n"
        "            (while (< k 760) (set (at s k) (+ (at s k) 1)) (set k (+ k 37))))))\n"
        "    count)",
        // a local stepped in the body, not last, inside a nested loop (mandel's k)
        "(fn f ((n u16)) u16 (let k u16 (& n 255))\n"
        "    (for y u8 0 9 (for x u8 0 31 (set (at t k) (+ x y)) (set k (+ k 1))\n"
        "        (set g (+ g (at t k)))))\n"
        "    k)",
        // stepped both ways, by bigger constants, in an arm, skipped by continue
        "(fn f ((n u16)) u16 (let k u16 (+ (& n 127) 300))\n"
        "    (for x u8 0 60 (set g (+ g (at t k))) (if (= (& x 3) 0) (set k (- k 5)))\n"
        "        (if (= (& x 7) 1) (continue)) (set k (+ k 9)) (set (at t k) x))\n"
        "    k)",
        "(fn f ((n u16)) u16 (let k u16 (& n 127))\n"
        "    (for x u8 0 3 (set g (+ g (at t k))) (set k (+ k 260)) (set g (^ g (at t k))))\n"
        "    k)",
        // through a pointer variable, stores and loads
        "(fn f ((n u16)) u16 (let k u16 (& n 511)) (let p (ptr u8) (addr_of (at t 2)))\n"
        "    (for x u8 0 50 (store (+ p k) (+ (load (+ p k)) 1)) (set k (+ k 3))) k)",
        // a u16 array and struct fields indexed by the counter in memory
        "(fn f ((n u16)) u16 (let sum u16 0)\n"
        "    (for i u16 (& n 63) 80 (let j u8 0) (while (< j 2) (set j (+ j 1)))\n"
        "        (set sum (+ sum (+ (at ps (as u8 i) b) (at w i))))) sum)",
        // the address of an element stepped as a let (search), into arrays of 1, 2, 4 bytes
        "(fn f ((n u16)) u16 (let found u16 999)\n"
        "    (for i u16 (& n 255) 500 (let p (ptr u8) (addr_of (at t i))) (let j u8 0)\n"
        "        (while (and (< j 3) (= (load (+ p j)) (at small j))) (set j (+ j 1)))\n"
        "        (if (= j 3) (do (set found i) (break))))\n"
        "    found)",
        "(fn f ((n u16)) u16 (let sum u16 0)\n"
        "    (for i u16 (& n 127) 299 (let q (ptr u16) (addr_of (at w i)))\n"
        "        (set sum (+ sum (load q)))) sum)",
        // the step pointer's value kept across a step in the same expression
        "(fn f ((n u16)) u16 (let k u16 (& n 255)) (let q (ptr u8) (addr_of (at t 0)))\n"
        "    (for x u8 0 20 (set q (addr_of (at t k))) (set k (+ k 1)) (set g (+ g (load q))))\n"
        "    k)",
        // the counter replaced by a let that moves with it: read as a value, as another
        // array's index, in a loop inside, after a continue; a start past the end; a u16 let
        "(fn f ((n u16)) u16 (let sum u16 0)\n"
        "    (for i u16 (& n 511) 700 (let p (ptr u8) (addr_of (at t i)))\n"
        "        (if (= (& (load p) 7) 3) (continue))\n"
        "        (if (= (load p) 77) (set sum (+ sum (* i 3))))\n"
        "        (set (at s (- i (& n 63))) (as u8 i))\n"
        "        (for j u8 0 2 (set sum (+ sum (+ i (as u16 j))))))\n"
        "    sum)",
        "(fn f ((n u16)) u16 (let sum u16 0)\n"
        "    (for i u16 n 400 (let q u16 (+ i 9)) (set sum (^ sum (+ q (>> i 1))))) sum)",
        // the race's search, written for t: the counter read once, where it is found
        "(fn f ((n u16)) u16 (set gw 0xFFFF)\n"
        "    (for i u16 0 1000 (let p (ptr u8) (addr_of (at t i))) (let j u8 0)\n"
        "        (while (and (< j 3) (= (load (+ p j)) (at t (+ (& n 511) (as u16 j)))))\n"
        "            (set j (+ j 1)))\n"
        "        (if (= j 3) (do (set gw i) (return i))))\n"
        "    0xFFFF)",
        "(fn f ((n u16)) u16 (let sum u16 0)\n"
        "    (for i u16 (+ n 65000) 65535 (let p (ptr u8) (addr_of (at t (& i 1023))))\n"
        "        (set sum (+ sum i))) sum)",
        // constant bounds: the difference and the stop are constants; a start past 0 and an
        // offset into the array, read as a value and on a match
        "(fn f ((n u16)) u16 (let sum u16 0)\n"
        "    (for i u16 37 700 (let p (ptr u8) (addr_of (at t (+ i 5))))\n"
        "        (if (= (load p) (as u8 n)) (set sum (+ sum i))) (set gw i)) (+ sum gw))",
        "(fn f ((n u16)) u16 (let r u16 0xFFFF)\n"
        "    (for i u16 300 1000 (let p (ptr u8) (addr_of (at t i)))\n"
        "        (if (= (load p) (as u8 n)) (do (set r i) (break)))) r)",
    };
    for (const char* body : bodies) {
        CheckCalls(results, body, Starts());
    }
}

// a let pointer worked out straight into its variable, also when the index reads it
void TestLetPointers(TestResults& results) {
    const char* bodies[] = {
        "(fn f ((x u8) (y u8)) u8 (let p (ptr u8) (addr_of (at t (+ (as u16 x) (as u16 y)))))\n"
        "    (load p))",
        "(fn f ((x u8) (y u8)) u8 (set gp (addr_of (at t (as u16 (load gp))))) (load gp))",
        "(fn f ((x u8) (y u8)) u8 (let p (ptr u8) (addr_of (at t (as u16 x))))\n"
        "    (set p (addr_of (at t (+ (as u16 (load p)) (as u16 y))))) (load p))",
    };
    for (const char* body : bodies) {
        CheckCalls(results, body, Bytes());
    }
}

// loop control: for loops whose counter the body never reads count down, while loops test
// before the loop instead of jumping to their test (a continue keeps the label), and branches
// on compares of known values are decided
void TestLoopControl(TestResults& results) {
    const char* bodies[] = {
        // counted down: 1, 2, 255 and 256 trips, a break, a continue, nested, in memory
        "(fn f ((x u8) (y u8)) u8 (let n u8 0)\n"
        "    (for k u8 0 1 (set n (+ n 3))) (for k u8 3 5 (set n (+ n x)))\n"
        "    (for k u8 0 255 (set n (+ n 1))) (for k u8 0 256 (set n (^ n y))) n)",
        "(fn f ((x u8) (y u8)) u8 (let n u8 0)\n"
        "    (for k u8 0 200 (set n (+ n 1)) (if (= n x) (break))\n"
        "        (if (= (& n 3) 1) (continue)) (set n (+ n 1))) n)",
        "(fn f ((x u8) (y u8)) u8 (let n u8 0)\n"
        "    (for a u8 0 10 (for b u8 0 7 (for c u8 0 3 (set n (+ n (at bt (+ n x))))))) n)",
        // unrolled: a break and a continue in the copies, a loop in them
        "(fn f ((x u8) (y u8)) u8 (let n u8 y)\n"
        "    (for k u8 0 6 (set n (+ n 1)) (if (= n x) (break)) (if (= (& n 3) 1) (continue))\n"
        "        (set n (+ n 2)))\n"
        "    (for k u8 0 3 (for j u8 0 (& x 3) (set n (^ n (at bt j))))) n)",
        "(fn f ((x u8) (y u8)) u16 (let n u16 0)\n"
        "    (for k u8 0 8 (set n (+ n (as u16 (at bt (& x 63))))) (for j u8 0 x (set n (+ n "
        "1))))\n"
        "    n)",
        // rotated whiles: zero trips, a continue, an and/or test, registers in the test
        "(fn f ((x u8) (y u8)) u8 (let a u8 x) (let n u8 0)\n"
        "    (while (and (< a 200) (!= (at bt a) 0)) (set n (+ n 1)) (set a (+ a 1)))\n"
        "    (+ a n))",
        "(fn f ((x u8) (y u8)) u8 (let a u8 x) (let n u8 0)\n"
        "    (while (or (< a 10) (= (& a 7) 3)) (set a (+ a 1)) (if (= (& a 1) 0) (continue))\n"
        "        (set n (+ n a)) (if (> a 250) (break))) (+ a n))",
        "(fn f ((x u8) (y u8)) u8 (let a u8 0) (let n u8 y)\n"
        "    (while (< a (& x 15)) (while (> n a) (set n (- n 1))) (set a (+ a 1))) (+ a n))",
        // compares of known values: the first pass of a loop entered with a known counter
        "(fn f ((x u8) (y u8)) u8 (let j u8 0)\n"
        "    (while (and (< j 6) (= (at bt (+ x j)) (at small j))) (set j (+ j 1)))\n"
        "    (if (= j 6) 1 (+ j 10)))",
        "(fn f ((x u8) (y u8)) u8 (let j u8 7) (let n u8 0)\n"
        "    (while (< j 6) (set n (+ n 1)) (set j (+ j 1))) (if (> j 6) (+ n 1) n))",
    };
    for (const char* body : bodies) {
        CheckCalls(results, body, Bytes());
    }
    // a counter the body never reads counts down to zero (too many trips to unroll)
    Compiled c = CompileText(std::string(kData) +
                             "(fn f ((x u8)) u8 (let n u8 x) (for k u8 0 40 (set n (+ n "
                             "n))) n)");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::DEX) + CountMn(c, "t.f", tisple::Mn::DEY) +
                           CountMn(c, "t.f", tisple::Mn::DEC) ==
                       1);
    // a nest of loops small enough to unroll one by one: the copies of the outer ones count,
    // so the nest stays within one loop's budget instead of 6 * 7 * 7 reads (a fuzz farm
    // program that took 47 KB)
    Compiled nest = CompileText(std::string(kData) +
                                "(fn f () void (for a u8 0 6 (for b i8 -4 3 (for c i8 -4 3 "
                                "(io_read u8 49189)))))");
    CHECK(results, CountMn(nest, "t.f", tisple::Mn::LDA) < 80);
}

// compares of a while test against a byte variable the loop never sets, done against it plus
// one: > and < (and <= and >=, which are not), v = 255 (> never holds: no trips; <= always
// does), v = 0, first and last in an and, alone, in
// a test too big to copy before the loop, v an outer loop's counter, break and continue; and
// the race's insertion sort, over data with 255s and 0s in it
void TestBumpedCompares(TestResults& results) {
    const char* bodies[] = {
        "(fn f ((x u8) (y u8)) u8 (let j u8 (& y 63)) (let n u8 0)\n"
        "    (while (and (!= j 0) (> (at bt j) x)) (set n (+ n 1)) (set j (- j 1))) (+ n j))",
        "(fn f ((x u8) (y u8)) u8 (let j u8 y) (let n u8 0)\n"
        "    (while (and (< n 40) (<= (at bt j) x)) (set g (at bt j)) (set n (+ n 1)) (set j (+ j "
        "1)))\n"
        "    (+ n j))",
        "(fn f ((x u8) (y u8)) u8 (let j u8 y) (let n u8 0)\n"
        "    (while (< x (at bt j)) (set g (^ (at bt j) g)) (set n (+ n 1)) (set j (+ j 7))) (+ n "
        "j))",
        "(fn f ((x u8) (y u8)) u8 (let j u8 y) (let n u8 0)\n"
        "    (while (and (< n 50) (>= x (at bt j))) (set g (at bt j)) (set n (+ n 1)) (set j (+ j "
        "3))\n"
        "        (if (= (& j 7) 2) (continue)) (if (= n x) (break))) (+ n j))",
        // too big to copy before the loop: a jump to the test at the bottom
        "(fn f ((x u8) (y u8)) u8 (let j u8 y) (let n u8 0)\n"
        "    (while (and (and (> (at bt j) x)\n"
        "        (!= (+ (at small (& j 15)) (^ (at bt (+ j 1)) (+ j n)))\n"
        "            (+ (at small (& n 15)) (^ (at bt (+ j 2)) (- n j)))))\n"
        "        (and (< n 30) (!= (at small (& (+ j n) 15)) 255)))\n"
        "        (set n (+ n 1)) (set j (+ j 1))) (+ n j))",
        // v an outer loop's counter, or a let, with the compared byte moved first
        "(fn f ((x u8) (y u8)) u8 (let n u8 0)\n"
        "    (for k u8 (& x 7) (& y 15) (let j u8 (+ k 100))\n"
        "        (while (and (!= j 0) (> (at bt j) k)) (set n (+ n (at bt j))) (set j (- j 9))))\n"
        "    n)",
        "(fn f ((x u8) (y u8)) u8 (let v u8 (at bt x)) (let j u8 y) (let n u8 0)\n"
        "    (while (and (!= j 0) (> (at small (& j 15)) v))\n"
        "        (set (at small (& (+ j 1) 15)) (at small (& j 15))) (set j (- j 1)) (set n (+ n "
        "1)))\n"
        "    (+ n (at small 3)))",
    };
    for (const char* body : bodies) {
        CheckCalls(results, body, Bytes());
    }
    // insertion sort of a slice of bt copied into s
    const char* sort =
        "(let ws (array u8 256))\n"
        "(fn f ((x u8) (y u8)) u16\n"
        "    (for i u8 0 255 (set (at ws i) (at bt (+ i x))))\n"
        "    (for i u8 1 (| y 1)\n"
        "        (let v u8 (at ws i))\n"
        "        (let j u8 i)\n"
        "        (while (and (!= j 0) (> (at ws (- j 1)) v))\n"
        "            (set (at ws j) (at ws (- j 1)))\n"
        "            (set j (- j 1)))\n"
        "        (set (at ws j) v))\n"
        "    (let sum u16 0) (for i u8 0 255 (set sum (+ (* sum 3) (as u16 (at ws i))))) sum)";
    CheckCalls(results, sort, Bytes());
    // the race kernel: the compared byte stays in A for the move
    Compiled c = CompileText("(let arr (array u8 256) :address 0x4000)\n"
                             "(fn main () void\n"
                             "    (for i u8 1 256 (let v u8 (at arr i)) (let j u8 i)\n"
                             "        (while (and (!= j 0) (> (at arr (- j 1)) v))\n"
                             "            (set (at arr j) (at arr (- j 1))) (set j (- j 1)))\n"
                             "        (set (at arr j) v)))\n");
    CHECK(results, CountMn(c, "t.main", tisple::Mn::LDA) <= 5);
}

} // namespace

void RunBackendWhileTests(TestResults& results) {
    TestWideWhile(results);
    TestByteWhile(results);
    TestFacts(results);
    TestStepPointers(results);
    TestReplacedCounter(results);
    TestLetPointers(results);
    TestLoopControl(results);
    TestBumpedCompares(results);
}

} // namespace tisple_tests
