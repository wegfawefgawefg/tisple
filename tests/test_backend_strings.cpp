// string literals: "HELLO" as a (ptr u8) to its bytes and a 0 in the interpreter and on the
// 6502, one table for equal texts, (const NAME (ptr u8) "...") like any use, tables no kept
// code reaches left out of the layout, compile-time calls that read strings, and the errors

#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

// the string tables (str#0, str#1, ...) of a program
int StringTables(const tisple::Program& program) {
    int count = 0;
    for (const tisple::Var& var : program.vars) {
        count += var.name.rfind("str#", 0) == 0 ? 1 : 0;
    }
    return count;
}

const char* const kCopy = "(const GREETING (ptr u8) \"HELLO\")\n"
                          "(let out (array u8 32))\n"
                          "(let pos u8 0)\n"
                          "(fn copy ((s (ptr u8))) u8\n"
                          "    (let n u8 0)\n"
                          "    (while (!= (load (+ s n)) 0)\n"
                          "        (set (at out pos) (load (+ s n)))\n"
                          "        (set pos (+ pos 1))\n"
                          "        (set n (+ n 1)))\n"
                          "    n)\n"
                          "(fn pick ((i u8)) (ptr u8)\n"
                          "    (match i (0 GREETING) (1 \"tab\\tquote\\\" back\\\\\") (_ \"\")))\n"
                          "(fn main () u8\n"
                          "    (let a u8 (copy GREETING))\n"
                          "    (let b u8 (copy \"HELLO\"))\n"
                          "    (let c u8 (copy (pick 1)))\n"
                          "    (let d u8 (copy (pick 2)))\n"
                          "    (+ (+ a b) (+ c d)))\n";

void TestRuns(TestResults& results) {
    CheckProgram(results, kCopy, "t.main", {});
    Compiled c = CompileText(kCopy);
    // GREETING and "HELLO" share a table; the other two have one each
    CHECK(results, c.ok && StringTables(c.built.program) == 3);
    Diff d;
    CHECK(results, StartDiff(d, c) && DiffCall(d, "t.main", {}));
    std::int32_t out = tisple::FindVar(c.built.program, "t.out");
    std::uint16_t at = c.code.layout.vars[static_cast<std::size_t>(out)].address;
    std::string text;
    for (int i = 0; i < 26; i++) {
        text += static_cast<char>(d.sim.machine->mem[at + i]);
    }
    CHECK(results, text == "HELLOHELLOtab\tquote\" back\\");
    // the interpreter has the same bytes, and the 0 after the last string was not copied
    CHECK(results, d.in.memory[at + 25] == 0x5C && d.sim.machine->mem[at + 26] == 0);
}

// a table only a dropped function names is not in the program; neither is a const table
// nothing reads
void TestDropped(TestResults& results) {
    const char* text = "(const UNUSED (array u8 4) (make (array u8 4) 1 2 3 4))\n"
                       "(let out u8 0)\n"
                       "(fn first ((s (ptr u8))) u8 (load s))\n"
                       "(fn never () u8 (first \"NEVER CALLED\"))\n"
                       "(fn main () void (set out (first \"KEPT\")))\n";
    Compiled c = CompileMainText(text);
    CHECK(results, c.ok);
    const tisple::Program& program = c.built.program;
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const tisple::Var& var = program.vars[v];
        bool dropped = c.code.layout.vars[v].dropped;
        if (var.name == "t.UNUSED" || var.name == "str#0") {
            CHECK(results, dropped && c.code.layout.vars[v].size == 0);
        }
        // (load "KEPT") is 'K' once first is inlined: only the interpreter, running the
        // program as written, needs the string, and has it inside the code
        if (var.name == "str#1") {
            CHECK(results, !dropped && c.code.layout.vars[v].interp_only);
            CHECK(results, c.code.layout.vars[v].size == 5);
        }
    }
    // no data at all but the frames (first is inlined at the mid level, never emptied: its
    // parameters keep bytes, for the interpreter); out is in zero page
    CHECK(results, c.code.data_bytes == c.code.mem_frames);
    Diff d;
    CHECK(results, StartDiff(d, c) && DiffCall(d, "t.main", {}));
}

// a function a macro calls while compiling reads a string like any other memory
void TestCompileTime(TestResults& results) {
    const char* text = "(const NAME (ptr u8) \"Tisple\")\n"
                       "(fn length ((s (ptr u8))) u8\n"
                       "    (let n u8 0)\n"
                       "    (while (!= (load (+ s n)) 0) (set n (+ n 1)))\n"
                       "    n)\n"
                       "(fn name_length () u8 (length NAME))\n"
                       "(macro measured () (name_length))\n"
                       "(const LENGTH u8 (measured))\n"
                       "(fn main () u8 LENGTH)\n";
    Built built = BuildText(text);
    std::string errors = ErrorText(built);
    if (!built.ok) {
        std::fprintf(stderr, "%s", errors.c_str());
    }
    CHECK(results, built.ok);
    CheckProgram(results, text, "t.main", {});
}

void TestErrors(TestResults& results) {
    ExpectError(results, "(fn f () (ptr u8) \"caf\\xE9\")",
                "t.tsp:1:19: a string in code is plain ASCII (0 to 127); put other bytes in a "
                "const table");
    ExpectError(results, "(const G u16 \"HI\")",
                "t.tsp:1:14: a string is a (ptr u8); write (const G (ptr u8) \"HI\")");
    ExpectError(results, "(const G (ptr i8) \"HI\")",
                "t.tsp:1:19: a string is a (ptr u8); write (const G (ptr u8) \"HI\")");
    ExpectError(results, "(fn f () void (let s (ptr i8) \"HI\"))",
                "t.tsp:1:31: let s needs a (ptr i8) but \"HI\" is (ptr u8)");
    ExpectError(results, "(fn f () void \"hi\" (do))",
                "t.tsp:1:15: \"hi\" on its own does nothing; remove it");
    ExpectError(results, "(const G (ptr u8) \"HI\")\n(macro m () G)\n(const N u16 (m))",
                "t.tsp:3:14: G is a (ptr u8); macro code reads integer and bool constants; "
                "macro code at t.tsp:2:13 (in macro m)");
}

} // namespace

void RunBackendStringTests(TestResults& results) {
    TestRuns(results);
    TestDropped(results);
    TestCompileTime(results);
    TestErrors(results);
}

} // namespace tisple_tests
