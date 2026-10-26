// carry facts on the 6502 against the interpreter: CLCs dropped where the carry is known
// clear (a counted loop's back edge, the guard, an add of small values), at the edges of
// each fact: a range that is almost true, writes that reach an array through pointers made
// or kept in every way, loops that exit early, signed counters

#include "test_backend.hpp"

#include <string>

namespace tisple_tests {

namespace {

// cells holds 0 or 1 from fill; sum reads three neighbours plus a constant
const char* const kCells =
    "(let cells (array u8 64))\n"
    "(let out (array u8 64))\n"
    "(fn fill ((k u8)) void\n"
    "    (for i u8 0 64 (set (at cells i) (& (* i k) 1))))\n"
    "(fn sum ((k u8)) void\n"
    "    (for i u8 1 63\n"
    "        (set (at out i)\n"
    "            (+ (+ (at cells (- i 1)) (at cells i)) (+ (at cells (+ i 1)) "
    "k)))))\n"
    "(fn sum2 () void\n"
    "    (for i u8 1 63\n"
    "        (set (at out i)\n"
    "            (+ (+ (at cells (- i 1)) (at cells i)) (+ (at cells (+ i 1)) "
    "252)))))\n";

void CheckCells(TestResults& results, const std::string& extra, const std::string& main_body) {
    std::string text = std::string(kCells) + extra + "(fn main () void\n    (fill 3)\n" +
                       main_body + "\n    (sum 250)\n    (sum2))\n";
    CheckProgram(results, text, "t.main", {});
}

void TestRanges(TestResults& results) {
    // all stores small: the sums cannot carry
    CheckCells(results, "", "");
    // a store of a large value anywhere breaks the range: directly, through a pointer
    // parameter, a returned pointer, a struct field, an array of pointers, a pointer to a
    // pointer, a pointer kept in a global
    CheckCells(results, "(fn big () void (set (at cells 5) 255))\n", "    (big)");
    CheckCells(results,
               "(fn put ((p (ptr u8)) (v u8)) void (store p v))\n"
               "(fn poke () void (put (addr_of (at cells 5)) 255))\n",
               "    (poke)");
    CheckCells(results,
               "(fn ptr_to ((i u8)) (ptr u8) (addr_of (at cells i)))\n"
               "(fn poke () void (store (ptr_to 6) 255))\n",
               "    (poke)");
    CheckCells(results,
               "(struct H ((n u8) (p (ptr u8))))\n"
               "(let h H)\n"
               "(fn poke () void\n"
               "    (set h.p (addr_of (at cells 7)))\n"
               "    (store h.p 255))\n",
               "    (poke)");
    CheckCells(results,
               "(let ps (array (ptr u8) 2))\n"
               "(fn poke () void\n"
               "    (set (at ps 1) (addr_of (at cells 9)))\n"
               "    (store (+ (at ps 1) 1) 255))\n",
               "    (poke)");
    CheckCells(results,
               "(let q (ptr u8))\n"
               "(fn poke () void\n"
               "    (let pp (ptr (ptr u8)) (addr_of q))\n"
               "    (store pp (addr_of (at cells 2)))\n"
               "    (store q 255))\n",
               "    (poke)");
    CheckCells(results,
               "(let g (ptr u8))\n"
               "(fn aim () void (set g (addr_of (at cells 40))))\n"
               "(fn poke () void (store (+ g 3) 255))\n",
               "    (aim)\n    (poke)");
    // a value just small enough, and one just too big: 253 + 1 + 1 + 1 carries only with
    // the 2 in a cell
    CheckCells(results, "(fn two () void (set (at cells 30) 2))\n", "    (two)");
    // a let that is almost always small, its set in a branch
    CheckProgram(results,
                 "(let out u8)\n"
                 "(fn g ((x u8) (y u8)) u8\n"
                 "    (let a u8 (& x 3))\n"
                 "    (let b u8 (& y 7))\n"
                 "    (if (> x 200) (set a 250))\n"
                 "    (+ (+ a b) (+ a 3)))\n"
                 "(fn main () void\n"
                 "    (set out (+ (g 1 2) (g 201 255))))",
                 "t.main", {});
    // a table of small constants, and a global that starts big
    CheckProgram(results,
                 "(const T (array u8 4) (make (array u8 4) 1 2 3 4))\n"
                 "(let w u8 200) (let o (array u8 8))\n"
                 "(fn main () void\n"
                 "    (for i u8 0 4\n"
                 "        (set (at o i) (+ (+ (at T i) (at T (& (+ i 1) 3))) (+ w 60)))))",
                 "t.main", {});
}

void TestLoopFacts(TestResults& results) {
    // a signed counter compares unsigned: the back edge does not leave the carry clear
    CheckProgram(results,
                 "(let s u8 0) (let t u16 0)\n"
                 "(fn main () void\n"
                 "    (for i i8 -4 4\n"
                 "        (set s (+ s 7))\n"
                 "        (set t (+ t (as u16 s)))))",
                 "t.main", {});
    // the carry set before the loop: the CLC moved before the top must still run there
    CheckProgram(results,
                 "(let s u8 0)\n"
                 "(fn h ((a u8) (b u8)) u8\n"
                 "    (set s (- a b))\n"
                 "    (for i u8 0 10 (set s (+ s 3)))\n"
                 "    s)",
                 "t.h", {200, 100});
    // a variable end, break and continue, a nested loop (the counter in memory)
    CheckProgram(results,
                 "(let s u8 0) (let w (array u8 16))\n"
                 "(fn main () void\n"
                 "    (for i u8 0 16 (set (at w i) (* i 17)))\n"
                 "    (for i u8 3 (at w 2)\n"
                 "        (if (= i 20) (continue))\n"
                 "        (set s (+ s (at w (& i 15))))\n"
                 "        (if (= s 77) (break))\n"
                 "        (for j u8 0 2\n"
                 "            (for k u8 0 2 (set s (+ s 1)))\n"
                 "            (set s (+ s (at w j))))))",
                 "t.main", {});
    // a loop through 255 (no compare at the end), then adds
    CheckProgram(results,
                 "(let s u8 0) (let t u8 0)\n"
                 "(fn main () void\n"
                 "    (for i u8 200 256 (set s (+ s i)))\n"
                 "    (for i u8 0 3 (set t (+ t s))))",
                 "t.main", {});
    // a jump table inside the loop, adds after it
    CheckProgram(results,
                 "(let s u8 0)\n"
                 "(fn main () void\n"
                 "    (for i u8 0 40\n"
                 "        (match (& i 3)\n"
                 "            (0 (set s (- s 3)))\n"
                 "            (1 (set s (+ s 200)))\n"
                 "            (2 (set s (- s 100)))\n"
                 "            (_ (set s (+ s i))))\n"
                 "        (set s (+ s 9))))",
                 "t.main", {});
}

} // namespace

void RunBackendCarryTests(TestResults& results) {
    TestRanges(results);
    TestLoopFacts(results);
}

} // namespace tisple_tests
