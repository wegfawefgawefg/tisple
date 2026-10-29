// (load (+ (addr_of (at a i)) k)) on byte arrays as a indexed by i + k (gen_place_sum.cpp),
// against the interpreter

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

void CheckRuns(TestResults& results, const std::string& text) {
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c);
    for (std::uint16_t x = 0; x < 256 && ok; x += 5) {
        ok = DiffCall(d, "t.f", {x, static_cast<std::uint16_t>((x * 7) & 0xFF)});
    }
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

void TestArrayPlusByte(TestResults& results) {
    const char* head =
        "(let a (array u8 64))\n(let b (array i8 300))\n(let w (array u16 8))\n"
        "(let g u8 9)\n"
        "(fn peek2 ((p (ptr u8)) (k u8)) u8 (load (+ p (as u16 (& k 3)))))\n"
        "(fn poke2 ((p (ptr u8)) (k u8) (v u8)) void (store (+ p (as u16 (& k 3))) v))\n"
        "(fn setup () void (for i u8 0 64 (set (at a i) (* i 3)))\n"
        "    (for j u16 0 300 (set (at b j) (as i8 (+ (* (as u8 j) 5) (* (as u8 (>> j 8)) "
        "99))))))\n";
    const char* bodies[] = {
        "(load (+ (addr_of (at a (& x 31))) (as u16 (& y 15))))",
        "(do (store (+ (addr_of (at a (& x 31))) (as u16 (& y 31))) g) (at a (& (+ x y) 63)))",
        "(do (poke2 (addr_of (at a (& x 31))) y 77) (peek2 (addr_of (at a (& y 31))) x))",
        // a u16 index into a bigger array, of i8
        "(as u8 (load (+ (addr_of (at b (as u16 (& x 127)))) (as u16 (& y 127)))))",
        // the sum may pass 255: a pointer as before
        "(as u8 (load (+ (addr_of (at b (as u16 x))) (as u16 (& y 31)))))",
        // the count worked out with code
        "(load (+ (addr_of (at a (& x 15))) (as u16 (& (+ (at a (& y 7)) y) 15))))",
    };
    for (const char* body : bodies) {
        CheckRuns(results, std::string(head) + "(fn f ((x u8) (y u8)) u8 (setup) " + body + ")");
    }
    Compiled c = CompileText(
        std::string(head) +
        "(fn f ((x u8) (y u8)) u8 (load (+ (addr_of (at a (& x 31))) (as u16 (& y 15)))))");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::TAX) + CountMn(c, "t.f", tisple::Mn::TAY) == 1);
    CHECK(results, CountMn(c, "t.f", tisple::Mn::STA) <= 1);
}

} // namespace

void RunBackendPlaceSumTests(TestResults& results) {
    TestArrayPlusByte(results);
}

} // namespace tisple_tests
