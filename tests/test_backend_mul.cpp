// multiplies on the 6502 against the interpreter: two bytes widened (the whole product of
// rt.mul8x8), a byte and a word (rt.mul16 with a zero high byte), words whose high bytes are
// zero only at run time, signed values, and the products of every pair of bytes

#include "test_backend.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace tisple_tests {

namespace {

const char* const kMul =
    "(let ta (array u8 256))\n"
    "(let tw (array u16 64))\n"
    "(let sum u16 0)\n"
    "(let mix u16 0)\n"
    "(fn setup () void\n"
    "    (for i u8 0 255 (set (at ta i) (^ (* i 37) 0x5A)))\n"
    "    (set (at ta 255) 255)\n"
    "    (for i u8 0 64 (set (at tw i) (+ (* (as u16 i) 1031) 7))))\n"
    // every byte b against a: both widened, and through table reads and masks
    "(fn bytes ((a u8)) u16\n"
    "    (set sum 0) (set mix 0)\n"
    "    (for b u8 0 255\n"
    "        (let p u16 (* (as u16 a) (as u16 b)))\n"
    "        (set sum (+ sum p)) (set mix (^ (+ mix (<< mix 1)) p)))\n"
    "    (let q u16 (* (as u16 a) 255))\n"
    "    (+ (+ sum mix) (+ q (* (as u16 (at ta a)) (as u16 (at ta (+ a 1)))))))\n"
    // a byte times a word, both ways round; words with zero high bytes at run time
    "(fn mixed ((a u8) (w u16)) u16\n"
    "    (let x u16 (* (as u16 a) w))\n"
    "    (let y u16 (* w (as u16 a)))\n"
    "    (let z u16 (* (& w 255) (& (+ w 3) 255)))\n"
    "    (let v u16 (* (>> w 8) (as u16 (at ta a))))\n"
    "    (let u u16 (* w (at tw (& a 63))))\n"
    "    (^ (+ x (* y 3)) (+ (+ z v) u)))\n"
    "(fn signed ((a i16) (b i16)) i16\n"
    "    (+ (* a b) (* (as i16 (as i8 a)) (as i16 (as i8 b)))))\n";

void TestProducts(TestResults& results) {
    Compiled c = CompileText(kMul);
    Diff d;
    bool ok = StartDiff(d, c) && DiffCall(d, "t.setup", {});
    for (std::uint16_t a = 0; a < 256 && ok; a++) {
        ok = DiffCall(d, "t.bytes", {a});
    }
    std::vector<std::uint16_t> words = {0,      1,      2,      0xFF,   0x100,  0x101,
                                        0x1FF,  0x7FFF, 0x8000, 0xFF00, 0xFFFF, 1234,
                                        0x00FE, 0xFE00, 0x55AA, 0xAA55, 300,    5678};
    for (int i : {0, 1, 2, 3, 127, 128, 200, 254, 255}) {
        std::uint16_t a = static_cast<std::uint16_t>(i);
        for (std::uint16_t w : words) {
            ok = ok && DiffCall(d, "t.mixed", {a, w});
            ok = ok && DiffCall(d, "t.signed", {w, static_cast<std::uint16_t>(i * 257)});
            ok = ok && DiffCall(d, "t.signed", {static_cast<std::uint16_t>(-i), w});
        }
    }
    if (!ok) {
        std::fprintf(stderr, "products:\n%s\n  %s\n", kMul, d.message.c_str());
    }
    CHECK(results, ok);
    // two widened bytes call the byte routine
    CHECK(results, CountMn(c, "t.bytes", tisple::Mn::JSR) >= 1);
    bool byte_routine = false;
    for (const tisple::AsmSymbol& s : c.code.as.symbols) {
        byte_routine = byte_routine || s.name == "rt.mul8x8";
    }
    CHECK(results, byte_routine);
}

} // namespace

void RunBackendMulTests(TestResults& results) {
    TestProducts(results);
}

} // namespace tisple_tests
