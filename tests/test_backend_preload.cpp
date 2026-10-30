// a store's index worked out in A goes straight into its register (PreloadIndex, TAX in
// LoadIndexReg), also Y for a store through a pointer, against the interpreter

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

void CheckRuns(TestResults& results, const std::string& text) {
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c);
    for (std::uint16_t x = 0; x < 256 && ok; x += 7) {
        ok = DiffCall(d, "t.f", {x, static_cast<std::uint16_t>(x ^ 0xA5)});
    }
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

void TestPreload(TestResults& results) {
    const char* head = "(struct S ((a u8) (b i16)))\n"
                       "(let a (array u8 16))\n(let w (array u16 8))\n(let s (array S 8))\n"
                       "(let g u8 3)\n(let h u16 0x1234)\n";
    const char* bodies[] = {
        "(set (at a (& x 15)) g)",
        "(set (at a (& x 15)) 77)",
        "(set (at w (& x 7)) h)",
        "(set (at s (& x 7) b) (as i16 h))",
        // the value has an index of its own
        "(set (at a (& x 15)) (at a (& y 15)))",
        "(set (at a (& x 15)) (+ g y))",
        // through a pointer, one a variable holds, the value a load through another
        "(store (+ (addr_of (at a 0)) (as u16 (& x 15))) g)",
        "(let p (ptr u8) (addr_of (at a (& y 7)))) (store (+ p (as u16 (& x 7))) g)",
        "(let p (ptr u8) (addr_of (at a (& y 7)))) (store (+ p (as u16 (& x 7))) (load p))",
        // in a loop whose counter is in X
        "(for i u8 0 (& y 7) (set (at a (& (+ x i) 15)) g) (set g (+ g i)))",
    };
    for (const char* body : bodies) {
        CheckRuns(results, std::string(head) + "(fn f ((x u8) (y u8)) u8 " + body +
                               " (+ (+ (at a (& x 15)) (at a (& y 15))) (as u8 (at w (& x 7)))))");
    }
    Compiled c = CompileText(std::string(head) + "(fn f ((x u8)) void (set (at a (& x 15)) g))");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::LDX) == 0);
    CHECK(results, CountMn(c, "t.f", tisple::Mn::TAX) == 1);
    Compiled p = CompileText(std::string(head) +
                             "(fn f ((x u8) (y u8)) void (let p (ptr u8) (addr_of (at a (& y 7))))"
                             " (store (+ p (as u16 (& x 7))) g))");
    CHECK(results, CountMn(p, "t.f", tisple::Mn::LDY) == 0);
    CHECK(results, CountMn(p, "t.f", tisple::Mn::TAY) == 1);
}

} // namespace

void RunBackendPreloadTests(TestResults& results) {
    TestPreload(results);
}

} // namespace tisple_tests
