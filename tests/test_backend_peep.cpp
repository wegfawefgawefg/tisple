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

} // namespace

void RunBackendPeepTests(TestResults& results) {
    TestDeadValues(results);
}

} // namespace tisple_tests
