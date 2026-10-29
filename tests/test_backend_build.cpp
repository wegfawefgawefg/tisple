// what `tisple build` leaves out: functions main never calls, and the runtime only they use

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

bool HasSpan(const tisple::Compiled& c, const std::string& name) {
    for (const tisple::FunctionSpan& span : c.spans) {
        if (span.name == name) {
            return true;
        }
    }
    return false;
}

bool HasSymbol(const tisple::Compiled& c, const std::string& name) {
    for (std::size_t i = 0; i < c.as.items.size(); i++) {
        const tisple::AsmItem& item = c.as.items[i];
        if (item.kind == tisple::ItemKind::Label &&
            c.as.symbols[static_cast<std::size_t>(item.value.sym)].name == name) {
            return true;
        }
    }
    return false;
}

std::int32_t FunctionId(const Compiled& c, const std::string& name) {
    return tisple::FindFunction(c.built.program, name);
}

void TestDroppedFunctions(TestResults& results) {
    // tiny is only inlined, big is called (twice, so it is not inlined as a function called
    // once), dead and dead2 are never called (dead2 only from dead), and leaf is called from
    // big; main's argument is read from a global, so the calls are not worked out at compile
    // time
    Compiled c = CompileText("(let out u16 0)\n"
                             "(fn tiny ((x u8)) u8 (+ x 1))\n"
                             "(fn leaf ((x u16)) u16\n"
                             "    (let a u16 (+ x 1)) (let b u16 (+ a x)) (let d u16 (+ b a))\n"
                             "    (let e u16 (+ d b)) (let f u16 (+ e d)) (let h u16 (+ f e))\n"
                             "    (+ h (/ x 7)))\n"
                             "(fn big ((x u16)) u16\n"
                             "    (let a u16 (leaf x)) (let b u16 (+ a x)) (let d u16 (+ b a))\n"
                             "    (let e u16 (+ d b)) (let f u16 (+ e d)) (let h u16 (+ f e))\n"
                             "    (+ h (leaf (+ x 3))))\n"
                             "(fn dead2 ((x u16) (y u16)) u16 (* x y))\n"
                             "(fn dead ((x u16)) u16\n"
                             "    (let a u16 (dead2 x x)) (let b u16 (+ a x)) (let d u16 (+ b a))\n"
                             "    (let e u16 (+ d b)) (let f u16 (+ e d)) (let h u16 (+ f e))\n"
                             "    (+ h (/ (dead2 x 3) 13)))\n"
                             "(fn main () u16 (set out (big (as u16 (tiny (as u8 out)))))\n"
                             "    (set out (+ out (big out))) out)");
    CHECK(results, c.ok);
    if (!c.ok) {
        std::fprintf(stderr, "%s\n", c.errors.c_str());
        return;
    }
    tisple::BuildOptions options;
    tisple::Compiled built;
    std::vector<tisple::Diagnostic> errors;
    CHECK(results, tisple::CompileProgram(c.built.program, options, built, errors));
    CHECK(results, HasSpan(built, "t.main"));
    CHECK(results, HasSpan(built, "t.big"));
    CHECK(results, HasSpan(built, "t.leaf"));
    CHECK(results, !HasSpan(built, "t.tiny"));
    CHECK(results, !HasSpan(built, "t.dead"));
    CHECK(results, !HasSpan(built, "t.dead2"));
    CHECK(results, built.function_address[static_cast<std::size_t>(FunctionId(c, "t.dead"))] == 0);
    // the multiply and the division by 13 were only dead code's (dead2's multiply runs twice
    // there, inlined twice, so it takes the quarter squares)
    CHECK(results, !HasSymbol(built, "rt.mul16") && !HasSymbol(built, "rt.mul16q"));
    CHECK(results, !HasSymbol(built, "rt.divu16_13"));
    CHECK(results, HasSymbol(c.code, "rt.mul16q") && HasSymbol(c.code, "rt.divu16_13"));
    CHECK(results, built.code_bytes < c.code.code_bytes);
    // what is left gives what the whole program gives, and that is the interpreter's
    tisple::SimRunner run;
    tisple::SimMain result = tisple::RunCompiledMain(run, built, 1000000);
    CHECK(results, result.finished && result.zp_restored);
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.main", {});
    std::uint16_t got = static_cast<std::uint16_t>(result.a | (result.x << 8));
    tisple::SimRunner all_run;
    tisple::SimMain all = tisple::RunCompiledMain(all_run, c.code, 1000000);
    CHECK(results, all.finished);
    CHECK(results, got == static_cast<std::uint16_t>(all.a | (all.x << 8)));
}

// with no main every function is kept: a library
void TestLibraryKeepsAll(TestResults& results) {
    Compiled c = CompileText("(fn f ((x u16)) u16 (/ x 13))\n"
                             "(fn g ((x u16) (y u16)) u16 (* x y))");
    CHECK(results, c.ok);
    tisple::Compiled built;
    std::vector<tisple::Diagnostic> errors;
    CHECK(results, tisple::CompileProgram(c.built.program, tisple::BuildOptions(), built, errors));
    CHECK(results, HasSpan(built, "t.f") && HasSpan(built, "t.g"));
    CHECK(results, built.code_bytes == c.code.code_bytes);
}

// jsr runs machine code: folding, hoisting and table reads never drop it or move it (the
// interpreter cannot run it, so these check the code)
void TestJsrStays(TestResults& results) {
    Compiled c =
        CompileText("(let g u8 0)\n(let lo (array u8 256))\n"
                    "(fn f () u8 (+ (* (jsr 0xFBDD) 0) 1))\n"
                    "(fn h () u8 (& (jsr 0x0300) 0))\n"
                    "(fn k () void (for x u8 0 4 (set g (+ (* (jsr 0x0300) 3) (* x 3)))))\n"
                    "(fn m () u8 (| (at lo (+ (jsr 0x0300) g)) (at lo (+ g 1))))");
    CHECK(results, c.ok);
    CHECK(results, CountMn(c, "t.f", tisple::Mn::JSR) == 1);
    CHECK(results, CountMn(c, "t.h", tisple::Mn::JSR) == 1);
    // the loop reads its counter and runs as four copies: one each
    CHECK(results, CountMn(c, "t.k", tisple::Mn::JSR) == 4);
    CHECK(results, CountMn(c, "t.m", tisple::Mn::JSR) == 1);
}

} // namespace

void RunBackendBuildTests(TestResults& results) {
    TestDroppedFunctions(results);
    TestLibraryKeepsAll(results);
    TestJsrStays(results);
}

} // namespace tisple_tests
