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
        // strlen: the step last, the test at the top
        "(fn f ((n u16)) u16 (while (!= (at s n) 0) (set n (+ n 1))) n)",
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
    // strlen's loop: no 16-bit add inside it
    Compiled c = CompileText(std::string(kData) +
                             "(fn f ((n u16)) u16 (while (!= (at s n) 0) (set n (+ n 1))) n)");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::INY) == 1);
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

} // namespace

void RunBackendWhileTests(TestResults& results) {
    TestWideWhile(results);
    TestByteWhile(results);
    TestFacts(results);
    TestStepPointers(results);
    TestLetPointers(results);
}

} // namespace tisple_tests
