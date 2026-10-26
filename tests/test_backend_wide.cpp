// 16-bit arithmetic with bytes known before running, on the 6502 against the interpreter:
// shifts of widened bytes, products and shifts that just fit a byte and just don't, adds that
// pass a byte through, into destinations that are their own operands

#include "test_backend.hpp"

#include <string>

namespace tisple_tests {

namespace {

// every u8 x through a list of u16 forms, each into its own table
void CheckForms(TestResults& results, const std::vector<std::string>& forms) {
    std::string text = "(let w u16 0x1234) (let v u16 0xFEDC)\n";
    for (std::size_t i = 0; i < forms.size(); i++) {
        text += "(let t" + std::to_string(i) + " (array u16 256))\n";
    }
    text += "(fn f () void\n    (for x u8 0 256\n";
    for (std::size_t i = 0; i < forms.size(); i++) {
        text += "        (set (at t" + std::to_string(i) + " x) " + forms[i] + ")\n";
    }
    text += "        (set w (+ w (as u16 x)))))\n";
    CheckProgram(results, text, "t.f", {});
}

void TestShifts(TestResults& results) {
    for (int c = 0; c < 16; c++) {
        std::string k = std::to_string(c);
        CheckForms(results, {
                                "(<< (as u16 x) " + k + ")",
                                "(>> (as u16 x) " + k + ")",
                                "(<< (as u16 (& x 7)) " + k + ")",
                                "(<< w " + k + ")",
                                "(>> w " + k + ")",
                                "(as u16 (>> (as i16 (as i8 x)) " + k + "))",
                                "(<< (+ (as u16 x) 0x100) " + k + ")",
                            });
    }
}

void TestProducts(TestResults& results) {
    CheckForms(results, {
                            "(* (as u16 (>> x 6)) 40)",
                            "(* (as u16 (& x 3)) 85)",
                            "(* (as u16 (& x 3)) 86)",
                            "(* (as u16 (& x 15)) 17)",
                            "(* (as u16 (& x 15)) 18)",
                            "(* (as u16 x) 42)",
                            "(* (as u16 (>> x 4)) 3)",
                        });
}

void TestAdds(TestResults& results) {
    CheckForms(results, {
                            "(+ (<< (as u16 x) 8) (as u16 (>> x 1)))",
                            "(+ 0x2000 (<< (as u16 (& x 7)) 10))",
                            "(+ (+ 0x2000 (<< (as u16 (& x 7)) 10)) (+ (<< (as u16 (& (>> x 3) 7)) "
                            "7) (* (as u16 (>> x 6)) 40)))",
                            "(+ 0x80FF (<< (as u16 x) 8))",
                            "(+ 0x01F0 (+ (<< (as u16 x) 8) 0x0020))",
                            "(+ (as u16 x) 0xFF00)",
                            "(+ (as u16 x) w)",
                            "(+ (<< w 8) (as u16 (as u8 (>> w 8))))",
                            "(+ (as u16 x) (<< (as u16 x) 8))",
                        });
    // the second operand just computed in A: commuted adds, ands, ors and xors
    CheckForms(results, {
                            "(+ w (* (as u16 x) 42))",
                            "(+ (| w 3) (<< (as u16 x) 3))",
                            "(& w (+ (as u16 x) 0x0F0F))",
                            "(^ (* (as u16 x) 3) w)",
                            "(| v (- w (as u16 x)))",
                            "(+ (* (as u16 x) 5) (* (as u16 x) 7))",
                            "(- w (* (as u16 x) 9))",
                        });
    // into destinations that are operands: w's bytes swapped, a byte passed in place
    CheckProgram(results,
                 "(let w u16 0x1234) (let v u16 0xABCD) (let s u16 0)\n"
                 "(fn f ((x u8)) u16\n"
                 "    (set w (+ (<< w 8) (as u16 (as u8 (>> w 8)))))\n"
                 "    (set v (+ (<< (as u16 x) 8) (as u16 (as u8 v))))\n"
                 "    (set s (+ (<< (as u16 (as u8 s)) 8) (as u16 (as u8 (>> s 8)))))\n"
                 "    (+ (+ w v) s))",
                 "t.f", {0x5A});
    // fuzz seed 39227 (before the generator changed): z is never written, so it is 0, but
    // z / 0 is all ones, not at most z
    CheckProgram(results,
                 "(let z u8) (let n u8) (let o u16)\n"
                 "(fn f () u16\n"
                 "    (set o (<< (as u16 (& (/ z n) 15)) 7))\n"
                 "    (+ o (<< (as u16 (/ z n)) 1)))",
                 "t.f", {});
    // a pointer plus a widened byte, and minus one
    CheckProgram(results,
                 "(let wb (array u8 600))\n"
                 "(fn f ((x u8)) u8\n"
                 "    (for i u16 0 600 (set (at wb i) (as u8 (* i 3))))\n"
                 "    (let p (ptr u8) (+ (addr_of (at wb 0)) (<< (as u16 (& x 1)) 8)))\n"
                 "    (let q (ptr u8) (+ (+ p (as u16 x)) (<< (as u16 (& x 1)) 8)))\n"
                 "    (+ (load p) (+ (load q) (load (- q (as u16 (>> x 1)))))))",
                 "t.f", {0xC7});
}

} // namespace

void RunBackendWideTests(TestResults& results) {
    TestShifts(results);
    TestProducts(results);
    TestAdds(results);
}

} // namespace tisple_tests
