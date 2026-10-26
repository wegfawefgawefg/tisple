// the register clean-ups on the 6502 against the interpreter: loads and ALU results nothing
// reads go, but never a read that may be I/O, and never one whose flags a branch reads

#include "test_backend.hpp"

#include <string>

namespace tisple_tests {

namespace {

void TestDeadValues(TestResults& results) {
    // reads of I/O whose values nobody uses stay, each one (the speaker, a soft switch)
    CheckProgram(results,
                 "(let wb (array u8 8))\n"
                 "(fn f ((x u8)) void\n"
                 "    (io_read u8 0xC030)\n"
                 "    (let p (ptr u8) (as (ptr u8) 0xC030))\n"
                 "    (load p)\n"
                 "    (io_read u8 0xC054)\n"
                 "    (let q (ptr u8) (addr_of (at wb (& x 7))))\n"
                 "    (load q)\n"
                 "    (io_read u16 0xC050))",
                 "t.f", {3});
    // flags of a load read after a label, and through a loop
    CheckProgram(results,
                 "(let wb (array u8 8) (make (array u8 8) 0 1 0 2 0 0 3 0)) (let n u8 0)\n"
                 "(fn id ((x u8)) u8 x)\n"
                 "(fn f () void\n"
                 "    (for i u8 0 8\n"
                 "        (if (= (at wb i) 0) (set n (+ n 1)))\n"
                 "        (if (!= (id (at wb i)) 0) (set n (+ n 16)))))",
                 "t.f", {});
    // a pointer nobody reads leaves no code; its value, read later, still does
    Compiled c = CompileText("(let wb (array u8 8)) (let g u8)\n"
                             "(fn f () void\n"
                             "    (let p (ptr u8) (addr_of (at wb 3)))\n"
                             "    (set g 1))");
    CHECK(results, c.ok && CountMn(c, "t.f", tisple::Mn::LDA) == 1);
    CheckProgram(results,
                 "(let wb (array u8 8)) (let g u8)\n"
                 "(fn f () u8\n"
                 "    (let p (ptr u8) (addr_of (at wb 3)))\n"
                 "    (set g 1)\n"
                 "    (store p 9)\n"
                 "    (load (+ p 0)))",
                 "t.f", {});
}

// a reload only for its flags goes when they already show A, else is AND #$FF; a byte stored
// from another is the same as it until either changes
void TestFlagReloads(TestResults& results) {
    const std::uint16_t values[] = {0, 1, 4, 5, 200};
    for (std::uint16_t x : values) {
        CheckProgram(results,
                     "(let g u8) (let h u8) (let n u8 0) (let wb (array u8 4))\n"
                     "(fn f ((x u8)) u8\n"
                     "    (set g x)\n"
                     "    (if (= g 0) (set n (+ n 1)))\n"
                     "    (if (< x 5) (set n (+ n 2)))\n"
                     "    (if (= x 0) (set n (+ n 4)))\n"
                     "    (if (!= x 4) (set n (+ n 8)))\n"
                     "    (if (and (< x 5) (= x 0)) (set n (+ n 32)))\n"
                     "    (if (and (>= x 4) (!= x 0)) (set n (+ n 64)))\n"
                     "    (set h g)\n"
                     "    (set g (+ x 1))\n"
                     "    (if (= h 0) (set n (+ n 16)))\n"
                     "    (set (at wb 1) h)\n"
                     "    (let p (ptr u8) (addr_of (at wb 1)))\n"
                     "    (store p 9)\n"
                     "    (set n (+ n (at wb 1)))\n"
                     "    (set h n)\n"
                     "    (set n 3)\n"
                     "    (+ h n))",
                     "t.f", {x});
    }
}

// an indexed store only changes its own array: an index register holding another byte keeps
// it, one holding a byte of that array does not
void TestIndexedStores(TestResults& results) {
    CheckProgram(results,
                 "(let a (array u8 8) (make (array u8 8) 0 1 2 3 4 5 6 7))\n"
                 "(let b (array u8 8)) (let i u8 3)\n"
                 "(struct S ((x u8) (y u8)))\n"
                 "(let ss (array S 8))\n"
                 "(fn f () u8\n"
                 "    (set (at a i) 5)\n"
                 "    (set (at b i) (at a i))\n"
                 "    (set (at ss i) (make S :x (at a i) :y i))\n"
                 "    (set (at a 2) 9)\n"
                 "    (set i (& (at a i) 7))\n"
                 "    (set (at a i) (+ (at b 3) i))\n"
                 "    (set (at a (at a 1)) 3)\n"
                 "    (set (at b (at a 1)) 1)\n"
                 "    (set (at a (at a 4)) 6)\n"
                 "    (set (at b (at a 4)) 2)\n"
                 "    (+ (at b 6) (at ss 5 x)))",
                 "t.f", {});
}

} // namespace

void RunBackendPeepTests(TestResults& results) {
    TestDeadValues(results);
    TestFlagReloads(results);
    TestIndexedStores(results);
}

} // namespace tisple_tests
