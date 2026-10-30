// Const data on the 6502 (src/check/data.cpp, src/opt/const_data.cpp, backend/link_data.cpp):
// a field of a const table of structs at a computed index is one indexed load; reads at
// constant places fold, and a const nothing kept reads is not in the binary; the programs agree
// with the interpreter at -O1 and -O0

#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

const char* const kEnemies =
    "(struct Move ((power u8) (w i16) (s (ptr u8))))\n"
    "(struct Enemy ((hp u8) (power u16) (name (ptr u8)) (kind u8) (moves (array Move 2))\n"
    "    (step (fn (u8) u8))))\n"
    "(fn double ((x u8)) u8 (* x 2))\n"
    "(fn half ((x u8)) u8 (/ x 2))\n"
    "(const BITE Move (make Move :power 3 :w -2 :s \"BITE\"))\n"
    "(const ENEMIES (array Enemy 4)\n"
    "    (make (array Enemy 4)\n"
    "        (make Enemy :hp 16 :power 300 :name \"PUFFCAP\" :kind 1\n"
    "            :moves (make (array Move 2) BITE (make Move :power 0 :w 300 :s \"HUNKER\"))\n"
    "            :step double)\n"
    "        (make Enemy :hp 11 :power 400 :name \"SPORELET\" :kind 3\n"
    "            :moves (make (array Move 2) (make Move :power 2 :w -7 :s \"SPORES\") BITE)\n"
    "            :step half)\n"
    "        (make Enemy :hp 9 :power 500 :name \"NIGHTFLAP\" :kind 1\n"
    "            :moves (make (array Move 2) BITE BITE) :step double)\n"
    "        (make Enemy :hp 30 :power 600 :name \"OWL\" :kind 7\n"
    "            :moves (make (array Move 2) BITE (at MOVES 1)) :step half)))\n"
    "(const MOVES (array Move 2) (make (array Move 2) BITE (make Move :power 6 :w 9 :s \"OW\")))\n"
    "(const NAMES (array (ptr u8) 3) (make (array (ptr u8) 3) \"A\" (+ \"XYZ\" 1)\n"
    "    (at ENEMIES 3 name)))\n"
    "(const BOSS (ptr Enemy) (addr_of (at ENEMIES 3)))\n"
    "(let cur (ptr Move) (addr_of (at (at ENEMIES 1 moves) 0)))\n"
    "(let out u8 0)\n"
    "(let wide u16 0)\n";

// the expressions read at index i of ENEMIES (i u8) and their sum in f
const char* const kReads =
    "(fn f ((i u8) (k u8)) u16\n"
    "    (let j u8 (& i 3))\n"
    "    (let m u8 (& k 1))\n"
    "    (let s u16 (+ (as u16 (at ENEMIES j hp)) (at ENEMIES j power)))\n"
    "    (set s (+ s (as u16 (load (+ (at ENEMIES j name) (as u16 (& k 3)))))))\n"
    "    (set s (+ s (as u16 (at (at ENEMIES j moves) m w))))\n"
    "    (set s (+ s (as u16 (load (at (at ENEMIES j moves) m s)))))\n"
    "    (let step (fn (u8) u8) (at ENEMIES j step))\n"
    "    (set s (+ s (as u16 (step k))))\n"
    "    (set s (+ s (as u16 (load (at NAMES (% k 3))))))\n"
    "    (set s (+ s (+ (as u16 (load BOSS hp)) (as u16 (load cur power)))))\n"
    "    (set cur (if (= m 0) (addr_of (at (at ENEMIES 3 moves) 1)) (+ cur 1)))\n"
    "    (match (at ENEMIES j kind)\n"
    "        (1 (set out (+ out 1)))\n"
    "        (5 (set out 99))\n"
    "        (_ (set out (^ out 3))))\n"
    "    s)\n";

void TestAgrees(TestResults& results) {
    std::string text = std::string(kEnemies) + kReads;
    for (int level : {1, 0}) {
        tisple::OptOptions opt;
        opt.level = level;
        Compiled c = CompileText(text, opt);
        Diff d;
        bool ok = StartDiff(d, c);
        for (std::uint16_t i = 0; i < 12 && ok; i++) {
            ok = DiffCall(d, "t.f", {i, static_cast<std::uint16_t>((i * 7 + 1) & 0xFF)});
        }
        if (!ok) {
            std::fprintf(stderr, "-O%d: %s\n", level, d.message.c_str());
        }
        CHECK(results, ok);
    }
}

bool HasAbsX(const Compiled& c, const std::string& function, tisple::Mn mn, int& count) {
    count = 0;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.name != function) {
            continue;
        }
        for (std::size_t i = span.first; i < span.end; i++) {
            const tisple::AsmItem& item = c.code.as.items[i];
            if (item.kind == tisple::ItemKind::Ins && item.mn == mn &&
                item.mode == tisple::Mode::AbsX) {
                count++;
            }
        }
    }
    return count > 0;
}

// split like RAM tables: a byte field is one LDA table,X; a u16 or pointer field two. An
// addr_of into the table (kEnemies has BOSS) packs it: then the index is multiplied
void TestIndexedLoads(TestResults& results) {
    const char* reads = "(fn hp ((i u8)) u8 (at ENEMIES (& i 3) hp))\n"
                        "(fn power ((i u8)) u16 (at ENEMIES (& i 3) power))\n"
                        "(fn name ((i u8)) (ptr u8) (at ENEMIES (& i 3) name))\n";
    Compiled c = CompileText("(struct Enemy ((hp u8) (power u16) (name (ptr u8))))\n"
                             "(const ENEMIES (array Enemy 4) (make (array Enemy 4)\n"
                             "    (make Enemy :hp 16 :power 300 :name \"PUFFCAP\")\n"
                             "    (make Enemy :hp 11 :power 400 :name \"SPORELET\")\n"
                             "    (make Enemy :hp 9 :power 500 :name \"NIGHTFLAP\")\n"
                             "    (make Enemy :hp 30 :power 600 :name \"OWL\")))\n" +
                             std::string(reads));
    CHECK(results, c.ok);
    int loads = 0;
    CHECK(results, HasAbsX(c, "t.hp", tisple::Mn::LDA, loads) && loads == 1);
    CHECK(results, HasAbsX(c, "t.power", tisple::Mn::LDA, loads) && loads == 2);
    CHECK(results, HasAbsX(c, "t.name", tisple::Mn::LDA, loads) && loads == 2);
    Diff d;
    bool ok = StartDiff(d, c);
    for (std::uint16_t i = 0; i < 4 && ok; i++) {
        ok = DiffCall(d, "t.hp", {i}) && DiffCall(d, "t.power", {i}) && DiffCall(d, "t.name", {i});
    }
    CHECK(results, ok);
    Compiled packed = CompileText(std::string(kEnemies) + reads);
    CHECK(results, packed.ok && !HasAbsX(packed, "t.hp", tisple::Mn::LDA, loads));
}

// a read at a constant place is its value; the table, never named by kept code, is not in
// the binary, at -O1 by the mid-level passes and at -O0 by the backend
void TestFolds(TestResults& results) {
    const char* text = "(struct E ((hp u8) (power u16)))\n"
                       "(const T (array E 3) (make (array E 3) (make E :hp 7 :power 0x1234)\n"
                       "    (make E :hp 8 :power 0x2345) (make E :hp 9 :power 0x3456)))\n"
                       "(let out u8 0)\n"
                       "(let wide u16 0)\n"
                       "(fn main () void (set out (at T 2 hp)) (set wide (at T 1 power)))\n";
    for (int level : {1, 0}) {
        tisple::OptOptions opt;
        opt.level = level;
        Compiled c = CompileText(text, opt);
        CHECK(results, c.ok);
        std::int32_t t = tisple::FindVar(c.built.program, "t.T");
        CHECK(results, t >= 0 && c.code.layout.vars[static_cast<std::size_t>(t)].dropped);
        int loads = 0;
        CHECK(results, !HasAbsX(c, "t.main", tisple::Mn::LDA, loads));
        Diff d;
        CHECK(results, StartDiff(d, c) && DiffCall(d, "t.main", {}));
    }
    // a string read from a table at a constant place: the load is the letter, and neither the
    // table nor the string takes bytes in the binary (the interpreter has the string in the code)
    Compiled names =
        CompileMainText("(const NAMES (array (ptr u8) 2) (make (array (ptr u8) 2) \"AB\" \"CD\"))\n"
                        "(let out u8 0)\n"
                        "(fn main () void (set out (load (+ (at NAMES 1) 1))))\n");
    CHECK(results, names.ok && names.code.data_bytes == names.code.mem_frames);
    Diff d;
    CHECK(results, StartDiff(d, names) && DiffCall(d, "t.main", {}));
}

// what the ranges pass knows of a field at an unknown index: its least and most, and its few
// values. Kinds are 1, 3, 1 and 7, so no clause for 5 is made and (= kind 2) is false
void TestRanges(TestResults& results) {
    Compiled c =
        CompileText(std::string(kEnemies) +
                    "(fn f ((i u8)) u8\n"
                    "    (match (at ENEMIES (& i 3) kind) (5 (set out 99)) (_ (set out 1)))\n"
                    "    (if (= (at ENEMIES (& i 3) kind) 2) 10 20))\n");
    CHECK(results, c.ok);
    CHECK(results, CountMn(c, "t.f", tisple::Mn::CMP) == 0);
    Diff d;
    bool ok = StartDiff(d, c);
    for (std::uint16_t i = 0; i < 4 && ok; i++) {
        ok = DiffCall(d, "t.f", {i});
    }
    CHECK(results, ok);
}

// an :address let outside the binary is written by the startup, which runs before addresses
// are known
void TestAddressOutside(TestResults& results) {
    ExpectLinkError(results, "(let p (ptr u8) \"HI\" :address 0x0300)\n(fn main () u8 (load p))",
                    "is outside the program's memory, so its value is numbers only");
}

} // namespace

void RunBackendDataTests(TestResults& results) {
    TestAgrees(results);
    TestIndexedLoads(results);
    TestFolds(results);
    TestRanges(results);
    TestAddressOutside(results);
}

} // namespace tisple_tests
