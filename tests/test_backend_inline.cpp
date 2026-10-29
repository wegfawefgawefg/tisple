// functions called in one place are inlined there, whatever their size (FindInlines)

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

// main's result and globals on the simulator are the interpreter's
void CheckMain(TestResults& results, const Compiled& c, const std::string& text) {
    CHECK(results, c.ok);
    if (!c.ok) {
        std::fprintf(stderr, "%s\n%s\n", c.errors.c_str(), text.c_str());
        return;
    }
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.main", {});
}

// work is far past the size inlined at every call; it returns early from inside its loop,
// takes a byte, a word and a pointer, and main calls it once, inside a loop, over edge values
const char* kOnce = "(let out u16 0)\n"
                    "(let buf (array u8 8))\n"
                    "(let vals (array u8 6) (make (array u8 6) 0 1 127 128 254 255))\n"
                    "(fn work ((a u8) (w u16) (p (ptr u8))) u16\n"
                    "    (let s u16 w)\n"
                    "    (for i u8 0 4\n"
                    "        (set s (+ s (as u16 (^ a (load (+ p (as u16 i)))))))\n"
                    "        (if (= (& s 0xFF) 0x80) (return (- s 1)))\n"
                    "        (store (+ p (as u16 i)) (+ a i)))\n"
                    "    (if (> a 200) (return (+ s 7)))\n"
                    "    (let t u16 (| (<< s 1) (>> s 15)))\n"
                    "    (set t (^ t (as u16 (- a 3))))\n"
                    "    (- t (as u16 (+ a 1))))\n"
                    "(fn main () u16\n"
                    "    (for k u8 0 6\n"
                    "        (let v u8 (at vals k))\n"
                    "        (set out (+ out (work v (+ out (as u16 v)) (addr_of (at buf 0))))))\n"
                    "    out)";

void TestOnceInlined(TestResults& results) {
    Compiled c = CompileMainText(kOnce);
    CheckMain(results, c, kOnce);
    CHECK(results, HasSpan(c.code, "t.main"));
    CHECK(results, !HasSpan(c.code, "t.work"));
    // named in no_inline it stays a function, giving the same
    tisple::BuildOptions options;
    options.no_inline.push_back("t.work");
    tisple::Compiled kept;
    std::vector<tisple::Diagnostic> errors;
    CHECK(results, tisple::CompileProgram(c.built.program, options, kept, errors));
    CHECK(results, HasSpan(kept, "t.work"));
    tisple::SimRunner a;
    tisple::SimRunner b;
    tisple::SimMain inlined = tisple::RunCompiledMain(a, c.code, 1000000);
    tisple::SimMain called = tisple::RunCompiledMain(b, kept, 1000000);
    CHECK(results, inlined.finished && called.finished);
    CHECK(results, inlined.a == called.a && inlined.x == called.x);
    CHECK(results, inlined.main_cycles < called.main_cycles);
}

// a big body, called once from where the code is made more than once: one copy at most is
// inlined, the other calls jump to the function
std::string Big(const std::string& name) {
    return "(fn " + name +
           " ((x u8)) u8\n"
           "    (let a u8 (+ x 3)) (let b u8 (^ a x)) (let c u8 (+ b (>> a 1)))\n"
           "    (let d u8 (- c (<< b 1))) (let e u8 (| d (& a 0x5A)))\n"
           "    (set g (+ g 1))\n"
           "    (+ e (^ d c)))\n";
}

void TestOnceInUnrolled(TestResults& results) {
    // the loop's counter is never read: four copies of its body
    std::string text = "(let g u8 0)\n(let out u8 0)\n" + Big("f") +
                       "(fn main () u8 (for i u8 0 4 (set out (+ out (f out)))) out)";
    Compiled c = CompileMainText(text);
    CheckMain(results, c, text);
    CHECK(results, HasSpan(c.code, "t.f"));
    // every copy calls it, none has the body
    CHECK(results, CountMn(c, "t.main", tisple::Mn::JSR) == 4);
}

void TestOnceInWhileTest(TestResults& results) {
    // a while test with a call is made once, at the bottom (gen_while.cpp): inlined there
    std::string text = "(let g u8 0)\n(let out u8 0)\n" + Big("f") +
                       "(fn main () u8\n"
                       "    (while (< (f out) 200) (set out (+ out 37)))\n"
                       "    (+ out g))";
    Compiled c = CompileMainText(text);
    CheckMain(results, c, text);
    CHECK(results, !HasSpan(c.code, "t.f"));
}

void TestOnceChain(TestResults& results) {
    // each calls the next once: inlined four deep, then a call
    std::string text = "(let g u8 0)\n(let out u8 0)\n";
    for (int i = 6; i >= 1; i--) {
        std::string next = i == 6 ? "x" : "(f" + std::to_string(i + 1) + " (+ x 1))";
        text += "(fn f" + std::to_string(i) +
                " ((x u8)) u8\n"
                "    (let a u8 (+ x 3)) (let b u8 (^ a x)) (let c u8 (+ b (>> a 1)))\n"
                "    (let d u8 (- c (<< b 1))) (let e u8 (| d (& a 0x5A)))\n"
                "    (set g (+ g " +
                std::to_string(i) + "))\n    (+ e (^ d " + next + ")))\n";
    }
    text += "(fn main () u8 (for i u8 0 3 (set out (+ out (f1 (+ out i))))) (+ out g))";
    Compiled c = CompileMainText(text);
    CheckMain(results, c, text);
    CHECK(results, !HasSpan(c.code, "t.f1") && !HasSpan(c.code, "t.f4"));
    CHECK(results, HasSpan(c.code, "t.f5"));
}

} // namespace

void RunBackendInlineTests(TestResults& results) {
    TestOnceInlined(results);
    TestOnceInUnrolled(results);
    TestOnceInWhileTest(results);
    TestOnceChain(results);
}

} // namespace tisple_tests
