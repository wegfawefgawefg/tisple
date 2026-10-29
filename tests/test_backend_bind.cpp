// inlined calls whose parameters read their argument variables where they are, on the 6502
// against the interpreter: and the cases that must still copy the argument in

#include "test_backend.hpp"

#include <string>

namespace tisple_tests {

namespace {

void TestBinding(TestResults& results) {
    const char* globals = "(let g u8 7) (let w u16 0x1234) (let v u8 3) (let out u16 0)\n"
                          "(let arr (array u8 16) (make (array u8 16) 9 8 7 6 5 4 3 2 1 0 1 2 3 4 "
                          "5 6))\n"
                          "(let q (ptr u8))\n";
    // the callee sets the argument variable, or its own parameter
    CheckProgram(results,
                 std::string(globals) + "(fn f ((a u8)) u8 (set g (+ g 1)) (+ a g))\n"
                                        "(fn h ((a u8)) u8 (set a (+ a 1)) (+ a a))\n"
                                        "(fn main () u16\n"
                                        "    (set out (as u16 (f g)))\n"
                                        "    (set out (+ out (as u16 (h g))))\n"
                                        "    (+ out (as u16 g)))",
                 "t.main", {});
    // a later argument sets the variable
    CheckProgram(results,
                 std::string(globals) + "(fn bump () u8 (set g (+ g 10)) 1)\n"
                                        "(fn two ((a u8) (b u8)) u8 (+ a (* b 3)))\n"
                                        "(fn main () u16 (as u16 (two g (bump))))",
                 "t.main", {});
    // the callee stores through a pointer to the variable
    CheckProgram(results,
                 std::string(globals) +
                     "(fn s ((a u8) (p (ptr u8))) u8 (store p 9) (+ a (load p)))\n"
                     "(fn main () u16 (as u16 (s v (addr_of v))))",
                 "t.main", {});
    // pointer, index and 16-bit parameters; nested inlining; a counter in memory
    CheckProgram(results,
                 std::string(globals) + "(fn rd ((p (ptr u8))) u8 (load (+ p 1)))\n"
                                        "(fn put ((p (ptr u8)) (x u8)) void (store (+ p 2) x))\n"
                                        "(fn at2 ((i u8)) u8 (at arr (& i 15)))\n"
                                        "(fn wide ((a u16) (b u16)) u16 (+ (- a b) (>> a 3)))\n"
                                        "(fn inner ((b u8)) u8 (+ b (at2 b)))\n"
                                        "(fn outer ((a u8)) u8 (inner a))\n"
                                        "(fn main () u16\n"
                                        "    (set q (addr_of (at arr 3)))\n"
                                        "    (put q (rd q))\n"
                                        "    (set out (wide w out))\n"
                                        "    (for i u8 0 4\n"
                                        "        (for j u8 0 3\n"
                                        "            (set out (+ out (as u16 (outer i))))\n"
                                        "            (set out (+ out (as u16 (at2 j))))))\n"
                                        "    (+ out (as u16 (outer v))))",
                 "t.main", {});
}

// a pointer only inlined calls dereference, through a parameter bound to it, two levels deep:
// it lives in zero page and the loads and stores go through it, never through a copy
void TestBoundPointers(TestResults& results) {
    std::string text =
        "(let arr (array u8 16) (make (array u8 16) 9 8 7 6 5 4 3 2 1 0 1 2 3 4 "
        "5 6))\n"
        "(let out u8 0)\n"
        "(fn peek ((p (ptr u8)) (k u8)) u8 (load (+ p (as u16 (& k 3)))))\n"
        "(fn poke ((p (ptr u8)) (k u8) (v u8)) void (store (+ p (as u16 (& k 3))) v))\n"
        "(fn twice ((p (ptr u8)) (k u8)) u8 (+ (peek p k) (peek p (+ k 1))))\n"
        "(fn f ((i u8)) u8\n"
        "    (let p (ptr u8) (addr_of (at arr (& i 7))))\n"
        "    (poke p i (twice p 2))\n"
        "    (set out (+ out (peek p 1)))\n"
        "    out)\n";
    Compiled c = CompileText(text);
    Diff d;
    CHECK(results, StartDiff(d, c));
    for (std::uint16_t i = 0; i < 12; i++) {
        CheckDiff(results, d, "t.f", {i});
    }
    if (!c.ok) {
        return;
    }
    int through = 0;
    bool copies = false;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.name != "t.f") {
            continue;
        }
        for (std::size_t k = span.first; k < span.end; k++) {
            const tisple::AsmItem& item = c.code.as.items[k];
            if (item.kind != tisple::ItemKind::Ins || item.value.sym < 0) {
                continue;
            }
            const std::string& name =
                c.code.as.symbols[static_cast<std::size_t>(item.value.sym)].name;
            through += item.mode == tisple::Mode::IndY && name == "t.f.p" ? 1 : 0;
            copies = copies || name == "t.f.ptrs";
        }
    }
    CHECK(results, through == 4);
    CHECK(results, !copies);
}

} // namespace

void RunBackendBindTests(TestResults& results) {
    TestBinding(results);
    TestBoundPointers(results);
}

} // namespace tisple_tests
