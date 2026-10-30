// the register clean-ups on the 6502 against the interpreter: loads and ALU results nothing
// reads go, but never a read that may be I/O, and never one whose flags a branch reads

#include "test_backend.hpp"

#include <cstdio>
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
        // arr is stored somewhere, so its reads are not its initial zeros
        "(fn fill ((x u8)) void (set (at arr (& x 7)) x))\n"
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

// a loop label holds what the fall, the jumps back and the jumps ahead into it all agree on.
// A jump ahead into a loop: the JMP over a continue that never runs, which the branch back
// then takes too (fuzz farm run 6, 227 programs)
void TestLoopEntries(TestResults& results) {
    const char* globals = "(let g u8 0) (let h u8 0) (let wb (array u8 1024))\n";
    const char* bodies[] = {
        // the store of 94 is not yet done on the way in
        "(fn f ((x u8)) u8 (for i u8 0 x (if (> g 255) (continue)) (set g 94)) g)",
        // the loads at the inner loop's top are not moved before its label, where only a fall
        // nothing reaches would run them
        ("(fn f ((x u8)) u8 (let k u16 0)\n"
         "    (for i u8 0 7 (if (> h 255) (set k (+ k 40))) (set g g)\n"
         "        (for m u8 0 2 (set (at wb k) (at wb k))))\n"
         "    (at wb (+ (as u16 x) 1)))"),
        // the branch back goes past the test A decides, to a label the branch in reaches
        ("(fn run ((x u8)) u8 (for i u8 0 x (set g (if (= g 2) 1 1))) (>> x x))\n"
         "(fn f ((x u8)) u8 (run x))"),
    };
    for (const char* body : bodies) {
        for (int x : {0, 1, 2, 3, 200}) {
            CheckProgram(results, std::string(globals) + body, "t.f",
                         {static_cast<std::uint16_t>(x)});
        }
    }
}

// a register still holds the indexed byte it was stored to or loaded from, until the index,
// that array, or (for (p),Y) any memory or the pointer changes
void TestIndexedReuse(TestResults& results) {
    const char* globals = "(struct Q ((a u8) (b u8) (c u8)))\n"
                          "(let g u8 0) (let h u8 0)\n"
                          "(let xs (array u8 64)) (let ys (array u8 64)) (let qs (array Q 32))\n"
                          "(let pa (ptr u8)) (let pb (ptr u8))\n"
                          "(fn setup () void\n"
                          "    (for i u8 0 64 (set (at xs i) (* i 3)) (set (at ys i) (+ i 100)))\n"
                          "    (for i u8 0 32 (set (at qs i a) i) (set (at qs i b) (* i 7)))\n"
                          "    (set pa (addr_of (at xs 0))) (set pb (addr_of (at xs 0))))\n";
    const char* bodies[] = {
        // the sprites: store, then compare twice
        "(fn f ((x u8) (y u8)) u8\n"
        "    (for i u8 0 64 (set (at xs i) (+ (at xs i) (at ys i)))\n"
        "        (if (or (< (at xs i) 8) (>= (at xs i) 240)) (set (at ys i) (- 0 (at ys i)))))\n"
        "    (at xs (& x 63)))",
        // a store to another element of the same array, the index changed, a store through a
        // pointer into the array
        "(fn f ((x u8) (y u8)) u8 (let i u8 (& x 63)) (let j u8 (& y 63))\n"
        "    (set (at xs i) 11) (set (at xs j) 22) (set g (at xs i))\n"
        "    (set (at xs i) 33) (set i (& (+ i 1) 63)) (set h (at xs i))\n"
        "    (set (at xs i) 44) (store (+ pa (as u16 j)) 55) (+ (+ g h) (at xs i)))",
        // an INC of the same array through the other index register (the counter in Y)
        "(fn f ((x u8) (y u8)) u8 (let j u8 (& y 63))\n"
        "    (for i u8 0 64 (set (at xs i) (+ x i)) (set (at xs j) (+ (at xs j) 1))\n"
        "        (set (at ys i) (at xs i)))\n"
        "    g)",
        // stores to other arrays and fields keep it; a field of the same struct array does not
        "(fn f ((x u8) (y u8)) u8 (let i u8 (& x 31)) (let j u8 (& y 31))\n"
        "    (set (at qs i a) (+ x 1)) (set (at ys j) 9) (set g (at qs i a))\n"
        "    (set (at qs i b) y) (set (at qs i c) (+ y 2)) (set h (at qs i b))\n"
        "    (set (at qs j b) 77) (+ (+ g h) (at qs i b)))",
        // (p),Y: kept while nothing is stored and the pointer stays; not across a store
        // through another pointer to the same bytes, nor a change of the pointer
        "(fn f ((x u8) (y u8)) u8 (let i u8 (& x 63)) (let j u8 (& y 63))\n"
        "    (store (+ pa (as u16 i)) 5) (set g (load (+ pa (as u16 i))))\n"
        "    (store (+ pa (as u16 i)) 6) (store (+ pb (as u16 j)) 7) (set h (load (+ pa (as u16 "
        "i))))\n"
        "    (store (+ pa (as u16 i)) 8) (set pa (+ pa 1)) (set g (+ g (load (+ pa (as u16 i)))))\n"
        "    (set pa (- pa 1)) (+ g h))",
        // in a loop over i with the index in a register, and a counter read back
        "(fn f ((x u8) (y u8)) u8\n"
        "    (for i u8 0 32 (set (at qs i b) (+ (at qs i b) x)) (set g (^ g (at qs i b)))\n"
        "        (set (at qs (& (+ i y) 31) b) i) (set h (+ h (at qs i b))))\n"
        "    (+ g h))",
    };
    for (const char* body : bodies) {
        std::string text = std::string(globals) + body;
        Compiled c = CompileText(text);
        Diff d;
        bool ok = StartDiff(d, c) && DiffCall(d, "t.setup", {});
        for (int x : {0, 1, 2, 5, 31, 32, 63, 64, 200, 255}) {
            for (int y : {0, 1, 5, 31, 63, 255}) {
                ok = ok && DiffCall(d, "t.f",
                                    {static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y)});
            }
        }
        if (!ok) {
            std::fprintf(stderr, "indexed reuse:\n%s\n  %s\n", text.c_str(), d.message.c_str());
        }
        CHECK(results, ok);
    }
}

// lz4's match copy: m is read only through its step pointer and dead after the loop, so its
// own steps go (dead_stores.cpp); read after the loop, they stay. The count down tests len's
// high byte only when its low byte is zero (zero_tests.cpp)
void TestFaintStepsAndCounts(TestResults& results) {
    const char* text = "(let out (array u8 700))\n"
                       "(fn setup () void (for i u16 0 700 (set (at out i) (as u8 (* i 7)))))\n"
                       "(fn f ((o u16) (off u16) (n u16)) u16\n"
                       "    (let m u16 (- o off))\n"
                       "    (let len u16 n)\n"
                       "    (while (!= len 0)\n"
                       "        (set (at out o) (at out m))\n"
                       "        (set o (+ o 1))\n"
                       "        (set m (+ m 1))\n"
                       "        (set len (- len 1)))\n"
                       "    o)\n"
                       "(fn g ((o u16) (off u16) (n u16)) u16\n"
                       "    (let m u16 (- o off))\n"
                       "    (let len u16 n)\n"
                       "    (while (!= len 0)\n"
                       "        (set (at out o) (+ (at out m) 1))\n"
                       "        (set o (+ o 1))\n"
                       "        (set m (+ m 1))\n"
                       "        (set len (- len 1)))\n"
                       "    (+ o (* m 3)))\n";
    Compiled c = CompileText(text);
    // o and both pointers stepped, m not
    CHECK(results, c.ok && CountMn(c, "t.f", tisple::Mn::INC) == 6);
    CHECK(results, c.ok && CountMn(c, "t.f", tisple::Mn::ORA) <= 1);
    Diff d;
    bool ok = StartDiff(d, c);
    for (int n : {0, 1, 2, 255, 256, 257, 300}) {
        for (int o : {200, 255, 256, 300}) {
            for (int off : {1, 4, 199}) {
                std::vector<std::uint16_t> args = {static_cast<std::uint16_t>(o),
                                                   static_cast<std::uint16_t>(off),
                                                   static_cast<std::uint16_t>(n)};
                ok = ok && DiffCall(d, "t.setup", {}) && DiffCall(d, "t.f", args) &&
                     DiffCall(d, "t.g", args);
            }
        }
    }
    if (!ok) {
        std::fprintf(stderr, "faint steps:\n%s\n  %s\n", text, d.message.c_str());
    }
    CHECK(results, ok);
}

void RunBackendPeepTests(TestResults& results) {
    TestFaintStepsAndCounts(results);
    TestDeadValues(results);
    TestIndexedReuse(results);
    TestLabelMerges(results);
    TestLoopEntries(results);
    TestDeadGlobalStores(results);
    TestFlagReloads(results);
    TestIndexedStores(results);
}

} // namespace tisple_tests
