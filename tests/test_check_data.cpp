// Static data (src/check/data.cpp): consts of any type, addresses the link knows as constant
// values in consts and top-level lets, laid out as RAM data is; read in the interpreter

#include "test_check.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

const char* const kGame =
    "(struct Move ((name (ptr u8)) (power u8) (glyph u8)))\n"
    "(struct Enemy ((name (ptr u8)) (hp u8) (moves (array Move 2)) (step (fn (u8) u8))))\n"
    "(fn double ((x u8)) u8 (* x 2))\n"
    "(fn half ((x u8)) u8 (/ x 2))\n"
    "(const BITE Move (make Move :name \"BITE\" :power 3 :glyph '!'))\n"
    "(const ENEMIES (array Enemy 3)\n"
    "    (make (array Enemy 3)\n"
    "        (make Enemy :name \"PUFFCAP\" :hp 16 :moves (make (array Move 2) BITE\n"
    "            (make Move :name \"HUNKER\" :power 0 :glyph '#')) :step double)\n"
    "        (make Enemy :name \"SPORELET\" :hp 11 :moves (make (array Move 2)\n"
    "            (make Move :name \"SPORES\" :power 2 :glyph '%') BITE) :step half)\n"
    "        (make Enemy :name \"NIGHTFLAP\" :hp 9 :moves (at ENEMIES_MOVES 0) :step double)))\n"
    "(const ENEMIES_MOVES (array (array Move 2) 1) (make (array (array Move 2) 1)\n"
    "    (make (array Move 2) BITE BITE)))\n"
    "(const BOSS (ptr Enemy) (addr_of (at ENEMIES 2)))\n"
    "(const SECOND_NAME (ptr u8) (at ENEMIES 1 name))\n"
    "(const SHOUT (ptr u8) (+ \"HEY YOU\" 4))\n"
    "(let current (ptr Enemy) (addr_of (at ENEMIES 0)))\n"
    "(let hello (ptr u8) \"HELLO\")\n"
    "(let buffer (array u8 4))\n"
    "(const SLOT (ptr u8) (addr_of (at buffer 2)))\n"
    "(struct Hero ((hp u8) (bark Move) (at u8)))\n"
    "(let hero Hero (make Hero :hp 30 :bark BITE :at 1))\n"
    "(fn hp ((at u8)) u8 (at ENEMIES at hp))\n"
    "(fn power ((i u8) (k u8)) u8 (at (at ENEMIES i moves) k power))\n"
    "(fn letter ((i u8) (k u8)) u8 (load (+ (at ENEMIES i name) (as u16 k))))\n"
    "(fn step ((i u8) (x u8)) u8 (let f (fn (u8) u8) (at ENEMIES i step)) (f x))\n"
    "(fn boss_hp () u8 (load BOSS hp))\n"
    "(fn next_hp () u8 (load (+ current 1) hp))\n"
    "(fn second () u8 (load (+ SECOND_NAME 1)))\n"
    "(fn shout () u8 (load SHOUT))\n"
    "(fn hello_o () u8 (load (+ hello 4)))\n"
    "(fn glyph () u8 (at (at ENEMIES 0 moves) 1 glyph))\n"
    "(fn bite () u8 BITE.power)\n"
    "(fn hero_bark () u8 (+ hero.bark.glyph hero.at))\n"
    "(fn poke ((v u8)) u8 (store SLOT v) (at buffer 2))\n";

void CheckValues(TestResults& results) {
    Built built = BuildText(kGame);
    CHECK(results, built.ok);
    if (!built.ok) {
        std::fprintf(stderr, "%s\n", ErrorText(built).c_str());
        return;
    }
    tisple::Interp in;
    StartInterp(in, built);
    CHECK(results, CallOk(results, in, "t.hp", {1}) == 11);
    CHECK(results, CallOk(results, in, "t.power", {0, 0}) == 3);
    CHECK(results, CallOk(results, in, "t.power", {1, 0}) == 2);
    CHECK(results, CallOk(results, in, "t.power", {2, 1}) == 3);
    CHECK(results, CallOk(results, in, "t.letter", {2, 3}) == 'H');
    CHECK(results, CallOk(results, in, "t.letter", {0, 7}) == 0);
    CHECK(results, CallOk(results, in, "t.step", {0, 10}) == 20);
    CHECK(results, CallOk(results, in, "t.step", {1, 10}) == 5);
    CHECK(results, CallOk(results, in, "t.boss_hp", {}) == 9);
    CHECK(results, CallOk(results, in, "t.next_hp", {}) == 11);
    CHECK(results, CallOk(results, in, "t.second", {}) == 'P');
    CHECK(results, CallOk(results, in, "t.shout", {}) == 'Y');
    CHECK(results, CallOk(results, in, "t.hello_o", {}) == 'O');
    CHECK(results, CallOk(results, in, "t.glyph", {}) == '#');
    CHECK(results, CallOk(results, in, "t.bite", {}) == 3);
    CHECK(results, CallOk(results, in, "t.hero_bark", {}) == '!' + 1);
    CHECK(results, CallOk(results, in, "t.poke", {77}) == 77);
}

// a const's value may hold its own address, and two consts each other's
void CheckSelfAddresses(TestResults& results) {
    Built built =
        BuildText("(struct Node ((v u8) (next (ptr Node))))\n"
                  "(const RING (array Node 3) (make (array Node 3)\n"
                  "    (make Node :v 1 :next (addr_of (at RING 1)))\n"
                  "    (make Node :v 2 :next (addr_of (at RING 2)))\n"
                  "    (make Node :v 3 :next (addr_of (at OTHER 0)))))\n"
                  "(const OTHER (array Node 1) (make (array Node 1)\n"
                  "    (make Node :v 9 :next (addr_of (at RING 0)))))\n"
                  "(let start (ptr Node) (addr_of start_node))\n"
                  "(let start_node Node (make Node :v 5 :next (addr_of (at RING 2))))\n"
                  "(fn walk ((n u8)) u16\n"
                  "    (let p (ptr Node) start)\n"
                  "    (let sum u16 0)\n"
                  "    (for i u8 0 n (set sum (+ sum (as u16 (load p v)))) (set p (load p "
                  "next)))\n"
                  "    sum)\n");
    CHECK(results, built.ok);
    if (!built.ok) {
        std::fprintf(stderr, "%s\n", ErrorText(built).c_str());
        return;
    }
    tisple::Interp in;
    StartInterp(in, built);
    // 5, then 3, 9, 1, 2, 3
    CHECK(results, CallOk(results, in, "t.walk", {6}) == 5 + 3 + 9 + 1 + 2 + 3);
}

void CheckErrors(TestResults& results) {
    ExpectError(results, "(const T (array u8 2) (make (array u8 2) 1 (at T 0)))",
                "t.tsp:1:44: T reads its own value while it is being built; only its address, "
                "(addr_of ...), may be used in it");
    ExpectError(results, "(const P (ptr u8) (+ \"AB\" 5))",
                "t.tsp:1:19: this pointer is outside the string (3 bytes); a constant pointer "
                "stays inside what it points to");
    ExpectError(results, "(let g u16 0)\n(let p (ptr u8) (as (ptr u8) (as u16 \"HI\")))",
                "t.tsp:2:17: a top-level let starts with a constant value; set p at the start of "
                "main instead");
    ExpectError(results, "(struct S ((a (array u8 2))))\n(fn f () void (let s S (make S :a 0)))",
                "t.tsp:2:35: an array value only starts a const or a top-level let; leave the "
                "struct literal out and fill a with a for loop");
    ExpectError(results, "(struct P ((x u8) (y u8)))\n(const O P (make P :y 1 :x 2))",
                "t.tsp:2:12: (make P :y 1 :x 2) fields out of order; write (make P :x 2 :y 1)");
    ExpectError(results, "(struct P ((x u8)))\n(const O P 5)",
                "t.tsp:2:12: 5 is a number; a P is needed here");
    ExpectError(results, "(const T (array u8 2) 5)",
                "t.tsp:1:23: T is (array u8 2), so its value is (make (array u8 2) v0 v1 ...)");
    ExpectError(results, "(let g u8 0)\n(const T (array u8 1) (make (array u8 1) g))",
                "t.tsp:2:42: g is not constant; an element of T is built from numbers, constants, "
                "operators and addresses: a function's name, a string, (addr_of x)");
    ExpectError(results,
                "(struct P ((x u8)))\n(const O P (make P :x 1))\n(fn f () void (set O.x 2))",
                "t.tsp:3:20: O.x is part of a constant; it cannot be set");
    ExpectError(results, "(const K u8 1)\n(const P (ptr u8) (addr_of K))",
                "t.tsp:2:28: K is a number with no address; give it storage with a top-level "
                "let, or make it a const table");
}

// const tables of structs are laid out as RAM ones: a table per byte, split unless an addr_of
// reaches into them
void CheckLayout(TestResults& results) {
    Built split = BuildText("(struct E ((hp u8) (power u16) (name (ptr u8))))\n"
                            "(const T (array E 3) (make (array E 3)\n"
                            "    (make E :hp 1 :power 0x1234 :name \"A\")\n"
                            "    (make E :hp 2 :power 0x5678 :name \"B\")\n"
                            "    (make E :hp 3 :power 0x9ABC :name \"A\")))\n"
                            "(fn f ((i u8)) u16 (at T i power))\n");
    CHECK(results, split.ok);
    std::int32_t t = tisple::FindVar(split.program, "t.T");
    const tisple::VarPlace& place = split.layout.vars[static_cast<std::size_t>(t)];
    CHECK(results, place.split && place.plane_stride == 3 && place.size == 15);
    tisple::Interp in;
    StartInterp(in, split);
    std::uint16_t base = place.address;
    CHECK(results, in.memory[base + 0] == 1 && in.memory[base + 2] == 3);
    // power: its low bytes, then its high bytes
    CHECK(results, in.memory[base + 3] == 0x34 && in.memory[base + 6] == 0x12);
    CHECK(results, in.memory[base + 5] == 0xBC && in.memory[base + 8] == 0x9A);
    // the names: a low-byte table and a high-byte table of the strings' addresses; equal
    // strings share one table
    std::uint16_t a0 = static_cast<std::uint16_t>(in.memory[base + 9] | in.memory[base + 12] << 8);
    std::uint16_t a2 = static_cast<std::uint16_t>(in.memory[base + 11] | in.memory[base + 14] << 8);
    CHECK(results, a0 == a2 && in.memory[a0] == 'A' && in.memory[a0 + 1] == 0);
    CHECK(results, CallOk(results, in, "t.f", {1}) == 0x5678);

    Built packed = BuildText("(struct E ((hp u8) (power u16)))\n"
                             "(const T (array E 2) (make (array E 2) (make E :hp 1 :power 0x1234)\n"
                             "    (make E :hp 2 :power 0x5678)))\n"
                             "(fn f () (ptr E) (addr_of (at T 1)))\n");
    CHECK(results, packed.ok);
    t = tisple::FindVar(packed.program, "t.T");
    const tisple::VarPlace& p = packed.layout.vars[static_cast<std::size_t>(t)];
    CHECK(results, !p.split && p.size == 6);
    StartInterp(in, packed);
    CHECK(results, CallOk(results, in, "t.f", {}) == p.address + 3);
    CHECK(results, in.memory[p.address + 3] == 2 && in.memory[p.address + 4] == 0x78);
}

} // namespace

void RunCheckDataTests(TestResults& results) {
    CheckValues(results);
    CheckSelfAddresses(results);
    CheckErrors(results);
    CheckLayout(results);
}

} // namespace tisple_tests
