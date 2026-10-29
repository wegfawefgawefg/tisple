// multiplies on the 6502 against the interpreter: two bytes widened (the whole product of
// rt.mul8x8), a byte and a word (rt.mul16 with a zero high byte), words whose high bytes are
// zero only at run time, signed values, and the products of every pair of bytes; each by
// shift and add (in code that runs once) and by quarter squares (in loops, and then everywhere)

#include "backend/analysis.hpp"
#include "test_backend.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace tisple_tests {

namespace {

// products that run once (no loops, nothing calls them): the shift-and-add routines
const char* const kMulOnce =
    "(let ta (array u8 256))\n"
    "(let tw (array u16 64))\n"
    "(let sum u16 0)\n"
    "(let mix u16 0)\n"
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
    // one product: of bytes widened, of bytes kept to 8 bits, of words
    "(fn wide ((a u8) (b u8)) u16 (* (as u16 a) (as u16 b)))\n"
    "(fn narrow ((a u8) (b u8)) u8 (* a b))\n"
    "(fn word ((a u16) (b u16)) u16 (* a b))\n";

// the same with loops, which take the quarter-square routines, and then every product does
// (k is 0: an operand the loop changes, so the product is not moved out of it)
const std::string kMul = std::string(kMulOnce) +
                         "(fn setup () void\n"
                         "    (for i u8 0 255 (set (at ta i) (^ (* i 37) 0x5A)))\n"
                         "    (set (at ta 255) 255)\n"
                         "    (for i u8 0 64 (set (at tw i) (+ (* (as u16 i) 1031) 7))))\n"
                         // every byte b against a: both widened, and through table reads
                         "(fn bytes ((a u8)) u16\n"
                         "    (set sum 0) (set mix 0)\n"
                         "    (for b u8 0 255\n"
                         "        (let p u16 (* (as u16 a) (as u16 b)))\n"
                         "        (set sum (+ sum p)) (set mix (^ (+ mix (<< mix 1)) p)))\n"
                         "    (let q u16 (* (as u16 a) 255))\n"
                         "    (+ (+ sum mix) (+ q (* (as u16 (at ta a)) (as u16 (at ta (+ a "
                         "1)))))))\n"
                         "(fn mixed_loop ((a u8) (w u16)) u16 (let r u16 0)\n"
                         "    (for k u8 0 1 (set r (mixed (+ a k) w))) r)\n"
                         "(fn signed_loop ((a i16) (b i16)) i16 (let r i16 0)\n"
                         "    (for k u8 0 1 (set r (signed (+ a (as i16 k)) b))) r)\n"
                         "(fn wide_loop ((a u8) (b u8)) u16 (let r u16 0)\n"
                         "    (for k u8 0 1 (set r (* (as u16 (+ a k)) (as u16 b)))) r)\n"
                         "(fn narrow_loop ((a u8) (b u8)) u8 (let r u8 0)\n"
                         "    (for k u8 0 1 (set r (* (+ a k) b))) r)\n"
                         "(fn word_loop ((a u16) (b u16)) u16 (let r u16 0)\n"
                         "    (for k u8 0 1 (set r (* (+ a (as u16 k)) b))) r)\n";

bool HasRoutine(const Compiled& c, const std::string& name) {
    for (const tisple::AsmItem& item : c.code.as.items) {
        if (item.kind == tisple::ItemKind::Label &&
            c.code.as.symbols[static_cast<std::size_t>(item.value.sym)].name == name) {
            return true;
        }
    }
    return false;
}

// loops: the program with loops, which also runs the _loop functions
void TestProducts(TestResults& results, const std::string& text, bool loops) {
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c) && (!loops || DiffCall(d, "t.setup", {}));
    for (std::uint16_t a = 0; a < 256 && ok && loops; a++) {
        ok = DiffCall(d, "t.bytes", {a});
    }
    std::vector<std::string> suffixes = {""};
    if (loops) {
        suffixes.push_back("_loop");
    }
    std::vector<std::uint16_t> words = {0,      1,      2,      0xFF,   0x100,  0x101,
                                        0x1FF,  0x7FFF, 0x8000, 0xFF00, 0xFFFF, 1234,
                                        0x00FE, 0xFE00, 0x55AA, 0xAA55, 300,    5678};
    for (int i : {0, 1, 2, 3, 127, 128, 200, 254, 255}) {
        std::uint16_t a = static_cast<std::uint16_t>(i);
        for (std::uint16_t w : words) {
            for (const std::string& s : suffixes) {
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
            for (const std::string& s : suffixes) {
                ok = ok && DiffCall(d, "t.wide" + s, {x, y}) && DiffCall(d, "t.narrow" + s, {x, y});
            }
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
            for (const std::string& s : suffixes) {
                ok = ok && DiffCall(d, "t.word" + s, {a, b});
            }
        }
        for (std::uint16_t b = 0; b < 256 && ok; b += 5) {
            const std::string& s = suffixes.back();
            ok = DiffCall(d, "t.word" + s, {a, b}) && DiffCall(d, "t.word" + s, {b, a});
        }
    }
    if (!ok) {
        std::fprintf(stderr, "products:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
    // every routine of its kind ran (two widened bytes call the byte routines), and none of
    // the other kind
    for (const char* name : {"rt.mul8", "rt.mul16", "rt.mul8x8"}) {
        CHECK(results, HasRoutine(c, name) == !loops);
        CHECK(results, HasRoutine(c, std::string(name) + "q") == loops);
    }
}

// every byte times every constant, in a loop and alone; a byte widened and a word times
// constants of every digit pattern up to 300, the powers of two and their neighbours, sums and
// differences of two powers, and a spread
void TestConstProducts(TestResults& results) {
    for (int c = 0; c < 256; c++) {
        std::string k = std::to_string(c);
        CheckProgram(results,
                     "(let p (array u8 256))\n"
                     "(fn f () void (for i u8 0 256 (set (at p i) (* i " +
                         k + "))))\n",
                     "t.f", {});
        CheckProgram(results, "(fn one ((a u8)) u8 (+ (* a " + k + ") (* " + k + " (+ a 7))))",
                     "t.one", {static_cast<std::uint16_t>(c * 89 % 256)});
    }
    std::vector<std::uint32_t> factors;
    for (std::uint32_t c = 0; c <= 300; c++) {
        factors.push_back(c);
    }
    for (std::uint32_t i = 1; i < 16; i++) {
        for (std::uint32_t j = 0; j < i; j++) {
            factors.push_back((1u << i) + (1u << j));
            factors.push_back((1u << i) - (1u << j));
        }
        factors.push_back(((1u << i) + 1) & 0xFFFF);
    }
    std::uint32_t x = 4321;
    for (int n = 0; n < 60; n++) {
        x = x * 1103515245u + 12345u;
        factors.push_back((x >> 8) & 0xFFFF);
    }
    std::string words = "(let dv (array u16 32) (make (array u16 32)";
    std::uint32_t y = 99;
    for (int n = 0; n < 32; n++) {
        y = y * 1103515245u + 12345u;
        words += " " + std::to_string(n < 4 ? n * 0x5555 : (y >> 12) & 0xFFFF);
    }
    words += "))\n";
    for (std::uint32_t c : factors) {
        std::string k = std::to_string(c & 0xFFFF);
        CheckProgram(results,
                     words +
                         "(let w (array u16 256)) (let v (array u16 32))\n"
                         "(fn f () void\n"
                         "    (for i u8 0 255 (set (at w i) (* (as u16 i) " +
                         k +
                         ")))\n"
                         "    (for i u8 0 32 (set (at v i) (* (at dv i) " +
                         k + "))))\n",
                     "t.f", {});
    }
}

// how often functions run: main once, a function called in a loop of 10 ten times, one called
// twice from it 20; a product outside loops in a function that runs more than once takes the
// quarter squares
void TestRunCounts(TestResults& results) {
    std::string text = "(let g u8 3) (let h u16 0)\n"
                       "(fn twice ((a u8)) u8 (* a g))\n"
                       "(fn step ((a u8)) u8 (+ (twice a) (twice (+ a 1))))\n"
                       "(fn once () void (set h (* (as u16 g) h)))\n"
                       "(fn main () void\n"
                       "    (once)\n"
                       "    (for i u8 0 10 (set g (step i))))\n";
    Compiled c = CompileMainText(text);
    CHECK(results, c.ok);
    if (!c.ok) {
        return;
    }
    const tisple::Program& program = c.built.program;
    std::vector<std::uint64_t> runs = tisple::FindRunCounts(program);
    CHECK(results, runs[static_cast<std::size_t>(program.main)] == 1);
    CHECK(results, runs[static_cast<std::size_t>(tisple::FindFunction(program, "t.once"))] == 1);
    CHECK(results, runs[static_cast<std::size_t>(tisple::FindFunction(program, "t.step"))] == 10);
    CHECK(results, runs[static_cast<std::size_t>(tisple::FindFunction(program, "t.twice"))] == 20);
    CHECK(results, HasRoutine(c, "rt.mul8q") && !HasRoutine(c, "rt.mul8"));
    // the 16-bit product that runs once takes them too, the tables being in
    CHECK(results, HasRoutine(c, "rt.mul16q") && !HasRoutine(c, "rt.mul16"));
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.main", {});
}

} // namespace

void RunBackendMulTests(TestResults& results) {
    TestRunCounts(results);
    TestProducts(results, kMul, true);
    TestProducts(results, kMulOnce, false);
    TestConstProducts(results);
}

} // namespace tisple_tests
