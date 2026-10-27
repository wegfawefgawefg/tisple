// lib/hgr's line, in pieces, on the 6502 against the interpreter: 16-bit compares done high
// byte first when A holds it, locals that are only ever 1 or -1 stepping a variable by INC or
// DEC, and inlined calls whose parameters read conversions of the caller's variables in place

#include "test_backend.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace tisple_tests {

namespace {

std::vector<std::uint16_t> Edges() {
    std::vector<std::uint16_t> out;
    for (int v : {0,      1,      2,      0x7F,   0x80,   0xFF,   0x100,  0x101,  0x17F,
                  0x180,  0x1FF,  0x3FFF, 0x4000, 0x7F00, 0x7FFF, 0x8000, 0x8001, 0x80FF,
                  0xBFFF, 0xC000, 0xFE80, 0xFF00, 0xFF7F, 0xFF80, 0xFFFE, 0xFFFF}) {
        out.push_back(static_cast<std::uint16_t>(v));
    }
    return out;
}

// every function with both arguments from Edges, after setup when there is one
void CheckPairs(TestResults& results, const std::string& text,
                const std::vector<std::string>& functions, const char* setup = nullptr) {
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c) && (!setup || DiffCall(d, std::string("t.") + setup, {}));
    for (const std::string& fn : functions) {
        for (std::uint16_t x : Edges()) {
            for (std::uint16_t y : Edges()) {
                if (ok && !DiffCall(d, "t." + fn, {x, y})) {
                    std::fprintf(stderr, "%s(%u %u): ", fn.c_str(), x, y);
                    ok = false;
                }
            }
        }
    }
    if (!ok) {
        std::fprintf(stderr, "steps:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

// a let worked out just before its compare leaves its high byte in A
void TestHighFirst(TestResults& results) {
    std::string text;
    std::vector<std::string> fns;
    int n = 0;
    for (const char* type : {"i16", "u16"}) {
        for (const char* cmp : {"(>= t y)", "(< t y)", "(> y t)", "(<= y t)"}) {
            for (const char* value : {"(<< x 1)", "(- x y)", "(+ x 1)"}) {
                std::string name = "h" + std::to_string(n++);
                text += "(fn " + name + " ((x " + type + ") (y " + type + ")) u8 (let t " + type +
                        " " + value + ")\n    (if " + cmp + " 1 2))\n";
                fns.push_back(name);
            }
        }
    }
    CheckPairs(results, text, fns);
    Compiled c = CompileText(text);
    CHECK(results, CountMn(c, "t.h0", tisple::Mn::EOR) == 1);
    CHECK(results, CountMn(c, "t.h0", tisple::Mn::SBC) == 0);
}

// (set v (+ v s)) for locals s only ever 1 or -1, of every size, either way round, crossing
// pages and wrapping; and a local that is also 2 sometimes, which is not one
void TestUnitSteps(TestResults& results) {
    std::string text =
        "(fn u0 ((x u16) (y u16)) u16 (let s u16 1) (if (>= y 0x8000) (set s 0xFFFF))\n"
        "    (let v u16 x) (for k u8 0 (as u8 (& y 7)) (set v (+ v s))) v)\n"
        "(fn u1 ((x u16) (y u16)) u16 (let s i16 -1) (if (= (& y 1) 1) (set s 1))\n"
        "    (let v i16 (as i16 x)) (for k u8 0 (as u8 (& y 15)) (set v (+ s v))) (as u16 v))\n"
        "(fn u2 ((x u16) (y u16)) u16 (let s u8 1) (if (>= y 0x100) (set s 255))\n"
        "    (let v u8 (as u8 x)) (for k u8 0 (as u8 (& y 3)) (set v (+ v s))) (as u16 v))\n"
        "(fn u3 ((x u16) (y u16)) u16 (let s i8 1) (if (= (& y 2) 2) (set s -1))\n"
        "    (let v u16 x) (let n u8 0)\n"
        "    (while (< n (as u8 (& y 7))) (set v (+ v (as u16 s))) (set n (+ n 1))) v)\n"
        "(fn u4 ((x u16) (y u16)) u16 (let s u16 1) (if (>= y 0x8000) (set s 2))\n"
        "    (let v u16 x) (for k u8 0 (as u8 (& y 7)) (set v (+ v s))) v)\n"
        // the step in a register for a while loop
        "(fn u5 ((x u16) (y u16)) u16 (let s u8 255) (if (< y 0x100) (set s 1))\n"
        "    (let v u8 (as u8 x)) (let n u8 (as u8 y))\n"
        "    (while (!= n 0) (set v (+ v s)) (set n (- n 1))) (+ (as u16 v) (as u16 s)))\n";
    CheckPairs(results, text, {"u0", "u1", "u2", "u3", "u4", "u5"});
    Compiled c = CompileText(text);
    CHECK(results, CountMn(c, "t.u0", tisple::Mn::ADC) == 0);
    CHECK(results, CountMn(c, "t.u4", tisple::Mn::ADC) > 0);
}

// lib/hgr's plot: its parameters read the caller's i16 variables through (as u16 x) and
// (as u8 y) where they are; a sign extension is not bytes in place, so it is still copied
void TestBoundConversions(TestResults& results) {
    std::string text =
        "(let tab (array u8 256))\n"
        "(let g u16 0)\n"
        "(fn setup () void (for i u8 0 255 (set (at tab i) (* i 3))))\n"
        "(fn pick ((x u16) (y u8)) u16\n"
        "    (+ (as u16 (at tab (as u8 x))) (+ (as u16 y) (>> x 8))))\n"
        "(fn wide ((w i16)) i16 (- w 1))\n"
        "(fn c0 ((a u16) (b u16)) u16 (let x i16 (as i16 a)) (let y i16 (as i16 b))\n"
        "    (pick (as u16 x) (as u8 y)))\n"
        "(fn c1 ((a u16) (b u16)) u16 (let p u8 (as u8 a))\n"
        "    (pick (as u16 p) (as u8 b)))\n"
        "(fn c2 ((a u16) (b u16)) u16 (let q i8 (as i8 a))\n"
        "    (as u16 (wide (as i16 q))))\n"
        "(fn c3 ((a u16) (b u16)) u16 (let x i16 (as i16 a)) (let y i16 (as i16 b))\n"
        "    (set g (pick (as u16 x) (as u8 y))) (set x (+ x 1))\n"
        "    (+ g (pick (as u16 x) (as u8 (+ y 1)))))\n";
    CheckPairs(results, text, {"c0", "c1", "c2", "c3"}, "setup");
    // c0 copies nothing into pick's parameters: its stores are x, y and pick's sum (copies
    // would add three more)
    Compiled c = CompileText(text);
    CHECK(results, CountMn(c, "t.c0", tisple::Mn::STA) <= 6);
}

} // namespace

void RunBackendStepsTests(TestResults& results) {
    TestHighFirst(results);
    TestUnitSteps(results);
    TestBoundConversions(results);
}

} // namespace tisple_tests
