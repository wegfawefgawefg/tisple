// short for loops whose body reads the counter run as copies, the counter a number in each
// (gen_unroll.cpp), and inlined calls whose argument is a number read it as one

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

void CheckRuns(TestResults& results, const std::string& text,
               const std::vector<std::uint16_t>& xs) {
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c);
    for (std::size_t i = 0; i < xs.size() && ok; i++) {
        ok = DiffCall(d, "t.f", {xs[i]});
        if (!ok) {
            std::fprintf(stderr, "x = %u\n", xs[i]);
        }
    }
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

int Steps(const Compiled& c, const std::string& function) {
    int n = 0;
    for (tisple::Mn mn : {tisple::Mn::INX, tisple::Mn::INY, tisple::Mn::DEX, tisple::Mn::DEY,
                          tisple::Mn::INC, tisple::Mn::DEC}) {
        n += CountMn(c, function, mn);
    }
    return n;
}

const std::vector<std::uint16_t> kXs = {0, 1, 2, 3, 7, 0x40, 0x7F, 0x80, 0xFE, 0xFF};

void TestCountedCopies(TestResults& results) {
    // u8, i8 and u16 counters, 2 to 16 trips and 17 (a loop), across 0 and the top
    const char* loops[] = {"(for i u8 0 4",      "(for i u8 250 255",     "(for i u8 3 19",
                           "(for i u8 3 20",     "(for i i8 -3 2",        "(for i i8 120 127",
                           "(for i u16 254 258", "(for i u16 65530 65535"};
    for (const char* head : loops) {
        std::string h = head;
        bool wide = h.find("u16") != std::string::npos;
        std::string idx = wide ? "(as u8 (& i 7))" : "(& (as u8 i) 7)";
        std::string byte = wide ? "(as u8 i)" : "(as u8 i)";
        CheckRuns(results,
                  "(let a (array u8 8))\n(let g u16 0)\n"
                  "(fn f ((x u8)) u16 (set g 0)\n    " +
                      h + " (set (at a " + idx + ") (+ x " + byte + "))\n" +
                      "        (set g (+ (* g 3) (as u16 (at a " + idx + "))))\n" +
                      "        (set g (^ g (<< (as u16 x) (& " + byte + " 7))))\n" +
                      "        (set g (+ g (/ (as u16 x) (| (as u16 " + byte + ") 1)))))\n    g)",
                  kXs);
        // break and continue in the copies
        CheckRuns(results,
                  "(let g u16 0)\n"
                  "(fn f ((x u8)) u16 (set g 0)\n    " +
                      h + " (if (= (& " + byte + " 3) (& x 3)) (continue))\n" +
                      "        (set g (+ g (as u16 " + byte + ")))\n" +
                      "        (if (> g (as u16 x)) (break)))\n    g)",
                  kXs);
    }
    // nested, both counters read, and a while inside reading the outer one
    CheckRuns(results,
              "(let g u16 0)\n"
              "(fn f ((x u8)) u16 (set g 0)\n"
              "    (for i u8 1 4 (for j u8 0 3 (set g (+ g (as u16 (* i (+ j x))))))\n"
              "        (let k u8 0) (while (< k i) (set k (+ k 1)) (set g (+ g 1))))\n"
              "    g)",
              kXs);
    // an inlined function given the counter: its parameter is the number
    CheckRuns(results,
              "(let a (array u8 64))\n(let g u16 0)\n"
              "(fn cell ((y u8) (x u8)) u8 (at a (+ (* y 8) (& x 7))))\n"
              "(fn f ((x u8)) u16 (set g 0) (for k u8 0 64 (set (at a k) (+ k x)))\n"
              "    (for i u8 0 8 (set g (+ g (as u16 (cell i (+ i x))))))\n    g)",
              kXs);
    // a parameter the callee sets is a copy of the number, not the number
    CheckRuns(results,
              "(let g u16 0)\n"
              "(fn h ((y u8)) u8 (set y (+ y 1)) (* y 3))\n"
              "(fn f ((x u8)) u16 (set g (as u16 (h 5)))\n"
              "    (for i u8 0 3 (set g (+ (+ g (as u16 (h i))) (as u16 (h (+ x i))))))\n    g)",
              kXs);
    // copies, no counter steps; a fixed address each
    Compiled c = CompileText("(let a (array u8 8))\n"
                             "(fn f ((x u8)) void (for i u8 0 4 (set (at a i) (+ x i))))");
    CHECK(results, Steps(c, "t.f") == 0);
    CHECK(results, CountMn(c, "t.f", tisple::Mn::STA) == 4);
    // 17 trips: a loop
    Compiled d = CompileText("(let a (array u8 32))\n"
                             "(fn f ((x u8)) void (for i u8 0 17 (set (at a i) (+ x i))))");
    CHECK(results, Steps(d, "t.f") > 0);
}

} // namespace

void RunBackendUnrollTests(TestResults& results) {
    TestCountedCopies(results);
}

} // namespace tisple_tests
