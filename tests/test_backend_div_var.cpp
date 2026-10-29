// division by variables and by signed constants on the 6502 against the interpreter: every
// pair of bytes, u8 and i8, and for words every divisor 0 to 300 and a spread of larger ones
// (signed edges such as -32768 / -1 among them) against dividends from every range the
// routines split on, as / and % pairs that divide once and each alone

#include "test_backend.hpp"

#include <string>

namespace tisple_tests {

namespace {

// one call per divisor b, each dividing every byte a; f as a pair, g and h one result each
void TestBytePairs(TestResults& results, const std::string& type) {
    std::string text = "(let q (array " + type + " 256)) (let r (array " + type +
                       " 256))\n"
                       "(let q2 (array " +
                       type + " 256)) (let r2 (array " + type +
                       " 256))\n"
                       "(fn f ((b " +
                       type +
                       ")) void\n"
                       "    (for i u8 0 256\n"
                       "        (let a " +
                       type + " (as " + type +
                       " i))\n"
                       "        (set (at q i) (/ a b))\n"
                       "        (set (at r i) (% a b))))\n"
                       "(fn g ((b " +
                       type +
                       ")) void\n"
                       "    (for i u8 0 256\n"
                       "        (set (at r2 i) (% (as " +
                       type + " i) b))))\n(fn h ((b " + type +
                       ")) void\n"
                       "    (for i u8 0 256\n"
                       "        (set (at q2 i) (/ (as " +
                       type + " i) b))))\n";
    Compiled c = CompileText(text);
    Diff d;
    CHECK(results, StartDiff(d, c));
    if (!c.ok) {
        std::fprintf(stderr, "%s\n", c.errors.c_str());
        return;
    }
    for (std::uint16_t b = 0; b < 256; b++) {
        CheckDiff(results, d, "t.f", {b});
        CheckDiff(results, d, "t.g", {b});
        CheckDiff(results, d, "t.h", {b});
    }
}

// dividends for the word tests: 0 to 300, the edges of each byte and sign, and a spread
std::vector<std::uint16_t> WordDividends() {
    std::vector<std::uint16_t> out;
    for (std::uint32_t v = 0; v <= 300; v++) {
        out.push_back(static_cast<std::uint16_t>(v));
    }
    for (std::uint32_t bit = 9; bit < 16; bit++) {
        std::uint32_t p = 1u << bit;
        for (std::uint32_t v : {p - 1, p, p + 1, p + 255, p + 256}) {
            out.push_back(static_cast<std::uint16_t>(v));
        }
    }
    for (std::uint32_t v : {0x7FFEu, 0x7FFFu, 0x8000u, 0x8001u, 0xFF00u, 0xFF01u, 0xFFFEu, 0xFFFFu,
                            0xFF80u, 0xFF7Fu, 0x80FFu, 0xFFu}) {
        out.push_back(static_cast<std::uint16_t>(v));
    }
    std::uint32_t x = 12345;
    while (out.size() < 500) {
        x = x * 1103515245u + 12345u;
        out.push_back(static_cast<std::uint16_t>(x >> 12));
    }
    return out;
}

// divisors: every one to 300, then pairs around powers of two, signed edges, a spread
std::vector<std::uint16_t> WordDivisors() {
    std::vector<std::uint16_t> out;
    for (std::uint32_t v = 0; v <= 300; v++) {
        out.push_back(static_cast<std::uint16_t>(v));
    }
    for (std::uint32_t bit = 9; bit < 16; bit++) {
        std::uint32_t p = 1u << bit;
        for (std::uint32_t v : {p - 1, p, p + 1}) {
            out.push_back(static_cast<std::uint16_t>(v));
        }
    }
    for (std::uint32_t v :
         {0x7FFFu, 0x8000u, 0x8001u, 0xFFFFu, 0xFFFEu, 0xFFFDu, 0xFF00u, 0xFF01u, 0xFFF9u, 0xFF9Cu,
          0x8080u, 0x4097u, 4097u, 1000u, 10000u, 0x1FFu, 0x200u, 0xC000u}) {
        out.push_back(static_cast<std::uint16_t>(v));
    }
    std::uint32_t x = 777;
    while (out.size() < 400) {
        x = x * 1103515245u + 12345u;
        out.push_back(static_cast<std::uint16_t>(x >> 9));
    }
    return out;
}

std::string MakeTable(const std::string& name, const std::string& type,
                      const std::vector<std::uint16_t>& values, bool is_signed) {
    std::string n = std::to_string(values.size());
    std::string text =
        "(let " + name + " (array " + type + " " + n + ") (make (array " + type + " " + n + ")";
    for (std::uint16_t v : values) {
        std::int32_t s = is_signed ? static_cast<std::int16_t>(v) : v;
        text += " " + std::to_string(s);
    }
    return text + "))\n";
}

void TestWordPairs(TestResults& results, const std::string& type, bool is_signed) {
    std::vector<std::uint16_t> dividends = WordDividends();
    std::string n = std::to_string(dividends.size());
    std::string text = MakeTable("dv", type, dividends, is_signed) + "(let q (array " + type + " " +
                       n + ")) (let r (array " + type + " " + n +
                       "))\n"
                       "(fn f ((b " +
                       type +
                       ")) void\n"
                       "    (for i u16 0 " +
                       n +
                       "\n"
                       "        (let a " +
                       type +
                       " (at dv i))\n"
                       "        (set (at q i) (/ a b))\n"
                       "        (set (at r i) (% a b))))\n"
                       "(fn g ((a " +
                       type + ") (b " + type + ")) " + type + " (/ a b))\n(fn h ((a " + type +
                       ") (b " + type + ")) " + type + " (% a b))\n";
    Compiled c = CompileText(text);
    Diff d;
    CHECK(results, StartDiff(d, c));
    if (!c.ok) {
        std::fprintf(stderr, "%s\n", c.errors.c_str());
        return;
    }
    for (std::uint16_t b : WordDivisors()) {
        CheckDiff(results, d, "t.f", {b});
    }
    const std::uint16_t as[] = {0x8000, 0x7FFF, 0xFFFF, 0, 1, 300};
    const std::uint16_t bs[] = {0xFFFF, 0, 1, 0x8000, 7, 0xFFF9};
    for (std::uint16_t a : as) {
        for (std::uint16_t b : bs) {
            CheckDiff(results, d, "t.g", {a, b});
            CheckDiff(results, d, "t.h", {a, b});
        }
    }
}

// every i8 by every constant, in a loop as a pair and alone, and outside a loop
void TestSignedByteConstants(TestResults& results) {
    for (int c = -128; c < 128; c++) {
        std::string k = std::to_string(c);
        std::string text = "(let q (array i8 256)) (let r (array i8 256)) (let r2 (array i8 256))\n"
                           "(fn f () void\n"
                           "    (for i u8 0 256\n"
                           "        (let a i8 (as i8 i))\n"
                           "        (set (at q i) (/ a " +
                           k + "))\n        (set (at r i) (% a " + k +
                           "))\n        (set (at r2 i) (% (- a 1) " + k +
                           "))))\n"
                           "(fn alone ((a i8)) i8 (+ (/ a " +
                           k + ") (% (+ a 1) " + k + ")))\n";
        Compiled compiled = CompileText(text);
        Diff d;
        CHECK(results, StartDiff(d, compiled));
        if (!compiled.ok) {
            std::fprintf(stderr, "%s\n", compiled.errors.c_str());
            continue;
        }
        CheckDiff(results, d, "t.f", {});
        const std::uint16_t as[] = {0, 1, 2, 0x7F, 0x80, 0x81, 0xFF, 0xF9, 0x64, 0x9C};
        for (std::uint16_t a : as) {
            CheckDiff(results, d, "t.alone", {a});
        }
    }
}

void TestSignedWordConstant(TestResults& results, std::int32_t c,
                            const std::vector<std::uint16_t>& dividends) {
    std::string k = std::to_string(c);
    std::string n = std::to_string(dividends.size());
    std::string text = MakeTable("dv", "i16", dividends, true) + "(let q (array i16 " + n +
                       ")) (let r (array i16 " + n +
                       "))\n"
                       "(fn f () void\n"
                       "    (for i u16 0 " +
                       n +
                       "\n"
                       "        (let a i16 (at dv i))\n"
                       "        (set (at q i) (/ a " +
                       k + "))\n        (set (at r i) (% a " + k +
                       "))))\n"
                       "(fn g ((a i16)) i16 (- (% a " +
                       k + ") (/ a " + k + ")))\n";
    Compiled compiled = CompileText(text);
    Diff d;
    CHECK(results, StartDiff(d, compiled));
    if (!compiled.ok) {
        std::fprintf(stderr, "%s\n", compiled.errors.c_str());
        return;
    }
    CheckDiff(results, d, "t.f", {});
    const std::uint16_t as[] = {0x8000, 0x8001, 0x7FFF, 0xFFFF, 0, 1, 0xFF80, 0x0080};
    for (std::uint16_t a : as) {
        CheckDiff(results, d, "t.g", {a});
    }
}

void TestSignedWordConstants(TestResults& results) {
    std::vector<std::uint16_t> dividends = WordDividends();
    dividends.resize(420);
    for (std::int32_t c = -130; c <= 130; c++) {
        if (c != 0) {
            TestSignedWordConstant(results, c, dividends);
        }
    }
    for (std::int32_t bit = 8; bit < 15; bit++) {
        TestSignedWordConstant(results, 1 << bit, dividends);
        TestSignedWordConstant(results, -(1 << bit), dividends);
    }
    for (std::int32_t c : {-32768, 32767, -32767, 1000, -1000, 4097, -4097, 300, -300}) {
        TestSignedWordConstant(results, c, dividends);
    }
}

// u8 constants inside a loop, one result alone: the inline steps with their way out
void TestByteConstantsAlone(TestResults& results) {
    for (int c : {3, 5, 7, 9, 10, 25, 63, 100, 127}) {
        std::string k = std::to_string(c);
        CheckProgram(results,
                     "(let q (array u8 256)) (let r (array u8 256))\n"
                     "(fn f () void\n"
                     "    (for i u8 0 256 (set (at q i) (/ i " +
                         k +
                         ")))\n"
                         "    (for i u8 0 256 (set (at r i) (% (+ i 3) " +
                         k + "))))",
                     "t.f", {});
    }
}

} // namespace

void RunBackendDivVarTests(TestResults& results) {
    TestBytePairs(results, "u8");
    TestBytePairs(results, "i8");
    TestWordPairs(results, "u16", false);
    TestWordPairs(results, "i16", true);
    TestSignedByteConstants(results);
    TestSignedWordConstants(results);
    TestByteConstantsAlone(results);
}

} // namespace tisple_tests
