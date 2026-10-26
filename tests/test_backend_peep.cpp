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

// stores to globals and to callees' parameters that nothing reads before the next store: a
// call, a jump out or the return reads them all; arrays read indexed anywhere, pointers to
// address-taken globals, and fixed addresses keep theirs
void TestDeadGlobalStores(TestResults& results) {
    const char* globals =
        "(let g u8 0) (let w u16 0) (let arr (array u8 8)) (let t u8 0)\n"
        "(let ag u8 0) (let pa (ptr u8))\n"
        "(let screen u8 :address 0x0400)\n"
        "(fn reader () u8 (+ g (at arr (& t 7))))\n"
        "(fn big ((x u8)) u8 (let a u8 (+ x 1)) (let b u8 (+ a x)) (let c u8 (+ b a))\n"
        "    (let d u8 (+ c b)) (let e u8 (+ d c)) (let h u8 (+ e d)) (let i u8 (+ h e))\n"
        "    (let j u8 (+ i h)) (+ j (+ i (+ g (at arr (& x 7))))))\n";
    const char* bodies[] = {
        // overwritten: only the last one stays
        "(fn f ((x u8)) u8 (set g 1) (set g (+ x 2)) (set w 7) (set w (as u16 x)) g)",
        // a call between reads them
        "(fn f ((x u8)) u8 (set g 1) (set t (big x)) (set g 2) (set (at arr 3) 5) (set t "
        "(reader))\n"
        "    (set (at arr 3) x) g)",
        // the return reads them, on every path
        "(fn f ((x u8)) u8 (set g 1) (if (> x 9) (return 4)) (set g 3) (if (= x 2) (set g 5)) x)",
        // in a loop: read on the way round, or only at the end
        "(fn f ((x u8)) u8 (for i u8 0 4 (set t (+ g i)) (set g x)) (for i u8 0 3 (set w (as u16 "
        "i)))\n"
        "    (set w 9) t)",
        // a pointer to an address-taken global, and a fixed address
        ("(fn f ((x u8)) u8 (set pa (addr_of ag)) (set ag 1) (set t (load pa)) (set ag 2)\n"
         "    (set screen 1) (set screen x) t)"),
        // a callee's parameter, written and then the call
        "(fn f ((x u8)) u8 (set t (big (+ x 1))) (set t (+ t (big x))) t)",
    };
    for (const char* body : bodies) {
        for (int x : {0, 2, 9, 10, 200}) {
            CheckProgram(results, std::string(globals) + body, "t.f",
                         {static_cast<std::uint16_t>(x)});
        }
    }
    Compiled c = CompileText(std::string(globals) +
                             "(fn f ((x u8)) void (set g 1) (set g (+ x 2)) (set w 7) (set w "
                             "(as u16 x)))");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::STA) == 3);
    Compiled k = CompileText(std::string(globals) +
                             "(fn f ((x u8)) void (set g 1) (set t (reader)) (set g 2))");
    CHECK(results, CountMn(k, "t.f", tisple::Mn::STA) == 3);
}

// what registers and memory hold after a label is what every forward jump and the fall
// into it agree on
void TestLabelMerges(TestResults& results) {
    const char* globals = "(let g u8 0) (let t u8 0) (let arr (array u8 8))\n";
    const char* bodies[] = {
        "(fn f ((x u8)) u8 (set t x) (if (> x 3) (set g x) (set g t)) (+ g t))",
        "(fn f ((x u8)) u8 (set t 5) (if (> x 3) (set t 5) (set t 6)) (+ t x))",
        "(fn f ((x u8)) u8 (set g x) (if (= x 0) (set g 1)) (if (> x 9) (set t g)) (+ g x))",
        ("(fn f ((x u8)) u8 (set (at arr 1) x)\n"
         "    (match (& x 3) (0 (set g x)) (1 (set g 2)) (_ (set g (at arr 1)))) (+ g (at arr "
         "1)))"),
        "(fn f ((x u8)) u8 (set g 0) (for i u8 0 x (if (> i 2) (break)) (set g (+ g i))) g)",
    };
    for (const char* body : bodies) {
        for (int x : {0, 1, 2, 3, 4, 9, 10, 200}) {
            CheckProgram(results, std::string(globals) + body, "t.f",
                         {static_cast<std::uint16_t>(x)});
        }
    }
}

void RunBackendPeepTests(TestResults& results) {
    TestDeadValues(results);
    TestLabelMerges(results);
    TestDeadGlobalStores(results);
    TestFlagReloads(results);
    TestIndexedStores(results);
}

} // namespace tisple_tests
