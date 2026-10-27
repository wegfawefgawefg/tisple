// 16-bit logic and compares by byte on the 6502 against the interpreter: (& x c) against 0
// testing only the bytes c has bits in, signed compares with constants high byte first,
// shifts of a variable into itself, and the CRC step (if (bit) (set v (^ (<< v 1) k))
// (set v (<< v 1))) shifting once before the branch

#include "test_backend.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace tisple_tests {

namespace {

std::vector<std::uint16_t> Edges() {
    std::vector<std::uint16_t> out;
    for (int v : {0,      1,      2,      0x7E,   0x7F,   0x80,   0x81,   0xFE,   0xFF,
                  0x100,  0x101,  0x17F,  0x180,  0x1FF,  0x7F00, 0x7F7F, 0x7FFE, 0x7FFF,
                  0x8000, 0x8001, 0x807F, 0x8080, 0xFE7F, 0xFE80, 0xFEFF, 0xFF00, 0xFF01,
                  0xFF7F, 0xFF80, 0xFF81, 0xFFFE, 0xFFFF, 0x1234, 0xA5A5}) {
        out.push_back(static_cast<std::uint16_t>(v));
    }
    return out;
}

void CheckEach(TestResults& results, const std::string& text,
               const std::vector<std::string>& functions) {
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c);
    for (const std::string& fn : functions) {
        for (std::uint16_t x : Edges()) {
            ok = ok && DiffCall(d, "t." + fn, {x});
        }
    }
    if (!ok) {
        std::fprintf(stderr, "bytewise:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

void TestMasks(TestResults& results) {
    std::string text = "(let g u8 0)\n(let gw u16 0)\n";
    std::vector<std::string> fns;
    int n = 0;
    for (int c : {0x8000, 0x4000, 0x0080, 0x0001, 0xFF00, 0x00FF, 0x0180, 0x8001, 0xFFFF, 0}) {
        std::string k = std::to_string(c);
        std::string name = "m" + std::to_string(n++);
        // both ways round, the value and through a branch, and the operand in memory
        text += "(fn " + name + " ((x u16)) u8 (set gw x)\n" + "    (+ (if (!= (& x " + k +
                ") 0) 1 0) (+ (if (= 0 (& " + k + " gw)) 2 0) (if (= (& gw " + k + ") 0) 4 0))))\n";
        fns.push_back(name);
    }
    for (int c : {0x80, 0x40, 0x01, 0xFF, 0x81}) {
        std::string k = std::to_string(c);
        std::string name = "m" + std::to_string(n++);
        text += "(fn " + name + " ((x u16)) u8 (set g (as u8 x))\n" + "    (+ (if (!= (& g " + k +
                ") 0) 1 0) (if (= (& " + k + " g) 0) 2 0)))\n";
        fns.push_back(name);
    }
    CheckEach(results, text, fns);
}

void TestSignedConstants(TestResults& results) {
    std::string text = "(let g i16 0)\n";
    std::vector<std::string> fns;
    int n = 0;
    for (int c : {-32768, -32767, -257, -256, -255, -129, -128, -127, -1, 0, 1, 127, 128, 255, 256,
                  257, 32766, 32767}) {
        std::string k = std::to_string(c);
        std::string name = "s" + std::to_string(n++);
        text += "(fn " + name + " ((x i16)) u8 (set g x)\n" + "    (+ (+ (if (< x " + k +
                ") 1 0) (if (>= g " + k + ") 2 0)) (+ (if (> x " + k + ") 4 0) (if (<= g " + k +
                ") 8 0))))\n";
        fns.push_back(name);
    }
    // the mandel guard: four compares in two ors
    text += "(fn guard ((x i16)) u8\n"
            "    (if (or (or (>= x 128) (<= x -128)) (or (>= (+ x 1) 128) (<= (- x 1) -128))) 1 "
            "0))\n";
    fns.push_back("guard");
    CheckEach(results, text, fns);
}

void TestShifts(TestResults& results) {
    std::string text = "(let g u16 0)\n(let h u8 0)\n";
    std::vector<std::string> fns;
    for (int c : {1, 2, 3, 4, 8, 9, 15}) {
        std::string k = std::to_string(c);
        std::string name = "sh" + k;
        text += "(fn " + name + " ((x u16)) u16 (let v u16 x) (let w i16 (as i16 x))\n" +
                "    (set v (<< v " + k + ")) (set g (>> g " + k + ")) (set w (>> w " + k +
                ")) (set g (+ g (+ v (as u16 w)))) g)\n";
        fns.push_back(name);
    }
    // CRC steps: u16 and u8, the bit set or clear, the shift on either side, both arms
    // computing more, an arm reading the variable outside the shift
    text += "(fn crc ((x u16)) u16 (let c u16 x)\n"
            "    (for k u8 0 8 (if (!= (& c 0x8000) 0) (set c (^ (<< c 1) 0x1021))\n"
            "        (set c (<< c 1)))) c)\n"
            "(fn crc2 ((x u16)) u16 (let c u16 x)\n"
            "    (for k u8 0 8 (if (= 0 (& 0x8000 c)) (set c (<< c 1))\n"
            "        (set c (^ 0x8005 (<< c 1))))) c)\n"
            "(fn crc8 ((x u16)) u16 (let c u8 (as u8 x))\n"
            "    (for k u8 0 8 (if (!= (& c 0x80) 0) (set c (^ (<< c 1) 0x07))\n"
            "        (set c (+ (<< c 1) h)))) (as u16 c))\n"
            "(fn mixed ((x u16)) u16 (let c u16 x)\n"
            "    (for k u8 0 3 (if (!= (& c 0x8000) 0) (set c (+ (<< c 1) (<< c 1)))\n"
            "        (set c (^ (<< c 1) c)))) c)\n"
            "(fn other ((x u16)) u16 (let c u16 x)\n"
            "    (for k u8 0 3 (if (!= (& c 0x4000) 0) (set c (^ (<< c 1) 0x1021))\n"
            "        (set c (<< c 1)))) c)\n";
    for (const char* fn : {"crc", "crc2", "crc8", "mixed", "other"}) {
        fns.push_back(fn);
    }
    CheckEach(results, text, fns);
    // the CRC step shifts once: one ASL and one ROL in the loop
    Compiled c = CompileText(text);
    CHECK(results, CountMn(c, "t.crc", tisple::Mn::ASL) == 1);
    CHECK(results, CountMn(c, "t.crc", tisple::Mn::ROL) == 1);
}

} // namespace

void RunBackendBytewiseTests(TestResults& results) {
    TestMasks(results);
    TestSignedConstants(results);
    TestShifts(results);
}

} // namespace tisple_tests
