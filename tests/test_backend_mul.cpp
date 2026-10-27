// multiplies on the 6502 against the interpreter: two bytes widened (the whole product of
// rt.mul8x8), a byte and a word (rt.mul16 with a zero high byte), words whose high bytes are
// zero only at run time, signed values, and the products of every pair of bytes; each by
// shift and add (outside loops) and by quarter squares (inside them)

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
    "    (+ (* a b) (* (as i16 (as i8 a)) (as i16 (as i8 b)))))\n"
    // the same in loops, which take the quarter-square routines
    "(fn mixed_loop ((a u8) (w u16)) u16 (let r u16 0) (for k u8 0 1 (set r (mixed a w))) r)\n"
    "(fn signed_loop ((a i16) (b i16)) i16 (let r i16 0) (for k u8 0 1 (set r (signed a b))) r)\n"
    // one product: of bytes widened, of bytes kept to 8 bits, of words; and in loops
    "(fn wide ((a u8) (b u8)) u16 (* (as u16 a) (as u16 b)))\n"
    "(fn narrow ((a u8) (b u8)) u8 (* a b))\n"
    "(fn word ((a u16) (b u16)) u16 (* a b))\n"
    "(fn wide_loop ((a u8) (b u8)) u16 (let r u16 0) (for k u8 0 1 (set r (* (as u16 a) (as u16 "
    "b)))) r)\n"
    "(fn narrow_loop ((a u8) (b u8)) u8 (let r u8 0) (for k u8 0 1 (set r (* a b))) r)\n"
    "(fn word_loop ((a u16) (b u16)) u16 (let r u16 0) (for k u8 0 1 (set r (* a b))) r)\n";

bool HasRoutine(const Compiled& c, const std::string& name) {
    for (const tisple::AsmItem& item : c.code.as.items) {
        if (item.kind == tisple::ItemKind::Label &&
            c.code.as.symbols[static_cast<std::size_t>(item.value.sym)].name == name) {
            return true;
        }
    }
    return false;
}

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
            for (const char* suffix : {"", "_loop"}) {
                std::string s = suffix;
                ok = ok && DiffCall(d, "t.mixed" + s, {a, w});
                ok = ok && DiffCall(d, "t.signed" + s, {w, static_cast<std::uint16_t>(i * 257)});
                ok = ok && DiffCall(d, "t.signed" + s, {static_cast<std::uint16_t>(-i), w});
            }
        }
    }
    // every pair of bytes, both ways and both widths
    for (std::uint32_t a = 0; a < 256 && ok; a++) {
        for (std::uint32_t b = 0; b < 256 && ok; b++) {
            std::uint16_t x = static_cast<std::uint16_t>(a);
            std::uint16_t y = static_cast<std::uint16_t>(b);
            ok = DiffCall(d, "t.wide", {x, y}) && DiffCall(d, "t.wide_loop", {x, y}) &&
                 DiffCall(d, "t.narrow", {x, y}) && DiffCall(d, "t.narrow_loop", {x, y});
        }
    }
    // words: pairs of edges and a spread, and every byte against each
    std::uint32_t seed = 777;
    for (int k = 0; k < 40; k++) {
        seed = seed * 1103515245u + 12345u;
        words.push_back(static_cast<std::uint16_t>(seed >> 10));
    }
    for (std::uint16_t a : words) {
        for (std::uint16_t b : words) {
            ok = ok && DiffCall(d, "t.word", {a, b}) && DiffCall(d, "t.word_loop", {a, b});
        }
        for (std::uint16_t b = 0; b < 256 && ok; b += 5) {
            ok = DiffCall(d, "t.word_loop", {a, b}) && DiffCall(d, "t.word_loop", {b, a});
        }
    }
    if (!ok) {
        std::fprintf(stderr, "products:\n%s\n  %s\n", kMul, d.message.c_str());
    }
    CHECK(results, ok);
    // every routine ran: two widened bytes call the byte routines
    for (const char* name :
         {"rt.mul8", "rt.mul8q", "rt.mul16", "rt.mul16q", "rt.mul8x8", "rt.mul8x8q"}) {
        CHECK(results, HasRoutine(c, name));
    }
}

} // namespace

void RunBackendMulTests(TestResults& results) {
    TestProducts(results);
}

} // namespace tisple_tests
