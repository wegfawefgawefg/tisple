// the random-program race's programs: the generator's race mode, and Tisple as C
// (src/race/race_c.hpp). That the C compilers build the C and agree is races/random.sh's
// job; here the C must be written, and say what Tisple says where C would not

#include "fuzz_gen.hpp"
#include "race/race_c.hpp"
#include "race/race_gen.hpp"
#include "test_check.hpp"
#include "test_race.hpp"

namespace tisple_tests {

namespace {

std::string CText(TestResults& results, const std::string& text) {
    Built built = BuildText(text);
    CHECK(results, built.ok);
    if (!built.ok) {
        std::fprintf(stderr, "%s\n", ErrorText(built).c_str());
        return "";
    }
    std::string out;
    std::string error;
    bool ok = tisple::WriteRaceC(built.program, out, error);
    CHECK(results, ok);
    if (!ok) {
        std::fprintf(stderr, "race c: %s\n", error.c_str());
    }
    return out;
}

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void ExpectC(TestResults& results, const std::string& c, const std::string& part) {
    bool found = Has(c, part);
    CHECK(results, found);
    if (!found) {
        std::fprintf(stderr, "race c: no \"%s\" in\n%s\n", part.c_str(), c.c_str());
    }
}

// FNV-1a of the programs of seeds 1 to 300: race mode changed none of them, and nothing
// may, since the fuzz farm's seeds name the programs they found
void TestGeneratorUnchanged(TestResults& results) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::uint64_t seed = 1; seed <= 300; seed++) {
        for (char c : GenerateProgram(seed)) {
            hash ^= static_cast<std::uint8_t>(c);
            hash *= 0x100000001b3ull;
        }
    }
    CHECK(results, hash == 0xb0814bc0effaafdeull);
}

void TestRacePrograms(TestResults& results) {
    int written = 0;
    for (std::uint64_t seed = 1; seed <= 150; seed++) {
        std::string text = GenerateRaceProgram(seed);
        CHECK(results, !Has(text, "io_read") && !Has(text, "io_write"));
        CHECK(results, Has(text, "(fn run ") && Has(text, "(fn init ") &&
                           Has(text, "(fn checksum ") && Has(text, "(let opaque u16 :address"));
        Built built = BuildText(text);
        CHECK(results, built.ok);
        if (!built.ok) {
            std::fprintf(stderr, "race seed %llu: %s\n", static_cast<unsigned long long>(seed),
                         ErrorText(built).c_str());
            continue;
        }
        std::string c;
        std::string error;
        bool ok = tisple::WriteRaceC(built.program, c, error);
        CHECK(results, ok);
        if (!ok) {
            std::fprintf(stderr, "race seed %llu: %s\n", static_cast<unsigned long long>(seed),
                         error.c_str());
            continue;
        }
        written += Has(c, "RACE_NOINLINE void t_checksum(void) {") &&
                   Has(c, "RACE_NOINLINE void t_init(void) {") && Has(c, "int main(void) {");
        CHECK(results, !tisple::RaceFeatures(built.program, nullptr).empty());
    }
    CHECK(results, written == 150);
}

// every value of a type the interpreter gives must come out of C the same way
void TestArithmetic(TestResults& results) {
    std::string c = CText(results, "(let a u8 200) (let b u8 100) (let x i16 -5) (let y i16 3)\n"
                                   "(let w u16 40000) (let s i8 -1) (let k u8 3)\n"
                                   "(fn main () u16\n"
                                   "    (set a (* a b)) (set x (+ x y)) (set x (* x y))\n"
                                   "    (set s (<< s k)) (set s (>> s 9)) (set a (<< a 8))\n"
                                   "    (set w (<< w (& k 15))) (set w (<< w k))\n"
                                   "    w)\n");
    // u8 * u8 overflows a 16-bit int: through unsigned
    ExpectC(results, c, "t_a = ((uint8_t)((unsigned)t_a * t_b));");
    // i16 + and * wrap through unsigned
    ExpectC(results, c, "t_x = ((int16_t)((uint16_t)t_x + (uint16_t)t_y));");
    ExpectC(results, c, "t_x = ((int16_t)((unsigned)t_x * (unsigned)t_y));");
    // a signed shift left is done unsigned; >> 9 on an i8 is >> 7; << 8 on a u8 is 0
    ExpectC(results, c, "((int8_t)((unsigned)(uint8_t)t_s << tmp0))");
    ExpectC(results, c, "t_s = ((int8_t)(t_s >> 7));");
    ExpectC(results, c, "t_a = ((uint8_t)0);");
    // a count below 16 needs no test; one that may be 16 or more does
    ExpectC(results, c, "t_w = ((uint16_t)((unsigned)t_w << ((uint8_t)(t_k & 15))));");
    ExpectC(results, c, " > 15 ? (uint16_t)0 : ");
    ExpectC(results, c, "static uint16_t t_w = 40000u;");
}

void TestDivision(TestResults& results) {
    std::string c = CText(results, "(let a u8 7) (let b u8) (let x i16 -32768) (let y i16 -1)\n"
                                   "(fn main () i16\n"
                                   "    (set a (/ a b)) (set a (% a (| b 1))) (set a (/ a 0))\n"
                                   "    (set x (/ x y)) (set x (% x 7))\n"
                                   "    x)\n");
    // a variable divisor may be 0: all ones, or the dividend for %
    ExpectC(results, c, "tmp1 == 0 ? 255 : ((uint8_t)(tmp0 / tmp1))");
    // (| b 1) is never 0
    ExpectC(results, c, "t_a = ((uint8_t)(t_a % ((uint8_t)(t_b | 1))));");
    // a literal 0: all ones
    ExpectC(results, c, "t_a = ((uint8_t)(255));");
    // -32768 / -1 overflows a 16-bit int; Tisple wraps it
    ExpectC(results, c, " == -1 ? ((int16_t)(0u - (unsigned)");
    ExpectC(results, c, "t_x = ((int16_t)(t_x % 7));");
}

void TestOrderAndControl(TestResults& results) {
    std::string c =
        CText(results, "(let g u8 1) (let arr (array u8 8))\n"
                       "(fn bump () u8 (set g (+ g 1)) g)\n"
                       "(fn main () u8\n"
                       "    (set g (+ g (bump)))\n"
                       "    (set (at arr (& g 7)) (bump))\n"
                       "    (for i u8 250 256 (set g (+ g i)))\n"
                       "    (for j u8 0 g (set g (+ g 1)))\n"
                       "    (while (< g 100) (match g (3 (break)) (_ (set g (+ g 7)))))\n"
                       "    g)\n");
    // g is read before bump changes it
    ExpectC(results, c, "tmp1 = t_g;\n    tmp0 = t_bump();\n    t_g = ((uint8_t)(tmp1 + tmp0));");
    // and the element is picked before the call
    ExpectC(results, c, "tmp2 = ((uint8_t)(t_g & 7));\n    t_arr[tmp2] = t_bump();");
    // through the type's largest value, and an end worked out once
    ExpectC(results, c, "} while (i++ != 255);");
    ExpectC(results, c, "tmp3 = t_g;\n    for (j = 0; j < tmp3; j++) {");
    // a break in a match leaves the loop, not the switch
    ExpectC(results, c, "goto brk");
}

void TestRefusals(TestResults& results) {
    Built built = BuildText("(fn main () u8 (io_read u8 0xC000))");
    CHECK(results, built.ok);
    std::string c;
    std::string error;
    CHECK(results, !tisple::WriteRaceC(built.program, c, error));
    CHECK(results, Has(error, "io_read"));
}

} // namespace

void RunRaceCTests(TestResults& results) {
    TestGeneratorUnchanged(results);
    TestRacePrograms(results);
    TestArithmetic(results);
    TestDivision(results);
    TestOrderAndControl(results);
    TestRefusals(results);
}

} // namespace tisple_tests
