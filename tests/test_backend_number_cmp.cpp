// compares with a number on the left are done with the number on the right (k < b is
// b >= k + 1), in conditions and in a for loop's guard (gen_cond.cpp, CompareJump)

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

void CheckAll(TestResults& results, const std::string& text) {
    std::vector<std::uint16_t> xs = {0,      1,      2,      0x7E,   0x7F,  0x80,
                                     0x81,   0xFE,   0xFF,   0x100,  0x101, 0x7FFE,
                                     0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF};
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c);
    for (std::uint16_t x : xs) {
        ok = ok && DiffCall(d, "t.f", {x});
    }
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

void TestEveryOp(TestResults& results) {
    struct Type {
        const char* name;
        std::vector<const char*> numbers;
    };
    const Type types[] = {
        {"u8", {"0", "1", "2", "126", "127", "128", "254", "255"}},
        {"i8", {"-128", "-127", "-1", "0", "1", "126", "127"}},
        {"u16", {"0", "1", "255", "256", "32767", "32768", "65534", "65535"}},
        {"i16", {"-32768", "-32767", "-256", "-1", "0", "1", "255", "32766", "32767"}},
    };
    for (const Type& t : types) {
        std::string n = t.name;
        for (const char* op : {"<", "<=", ">", ">=", "=", "!="}) {
            for (const char* k : t.numbers) {
                // the other side needs code (an add), so it is worked out in A
                CheckAll(results, "(fn f ((x u16)) u8 (if (" + std::string(op) + " " + k + " (as " +
                                      n + " (+ x 1))) 1 2))");
                CheckAll(results, "(let g " + n + " 0)\n(fn f ((x u16)) bool (set g (as " + n +
                                      " x)) (" + op + " " + k + " g))");
            }
        }
    }
}

void TestGuard(TestResults& results) {
    for (const char* start : {"0", "1", "3", "254", "255"}) {
        std::string s = start;
        CheckAll(results, "(let n u8 0)\n"
                          "(fn f ((x u16)) u8\n"
                          "    (set n 0)\n"
                          "    (for i u8 " +
                              s +
                              " (+ (as u8 x) 1) (set n (+ n i)))\n"
                              "    n)");
        CheckAll(results, "(let n u8 0)\n"
                          "(fn f ((x u16)) u8\n"
                          "    (set n 0)\n"
                          "    (for i i8 " +
                              (s == "254"   ? std::string("-2")
                               : s == "255" ? "-1"
                                            : s) +
                              " (as i8 (+ (as u8 x) 1)) (set n (+ n 1)))\n"
                              "    n)");
    }
    // the end worked out in A is compared there with 4, not stored and compared with 3
    Compiled c = CompileText("(let n u8 0)\n"
                             "(fn f ((x u8)) void (for i u8 3 (& x 7) (set n (+ n i))))");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::CMP) == 1);
    Compiled d = CompileText("(fn f ((x u8)) u8 (if (< 3 (+ x x)) 1 2))");
    CHECK(results, CountMn(d, "t.f", tisple::Mn::STA) == 0);
    // at the type's top the compare is decided: (< 255 b) never, (>= 255 b) always
    Compiled e = CompileText("(fn f ((x u8)) u8 (if (< 255 (+ x x)) 1 2))");
    CHECK(results, CountMn(e, "t.f", tisple::Mn::CMP) == 0);
}

} // namespace

void RunBackendNumberCmpTests(TestResults& results) {
    TestEveryOp(results);
    TestGuard(results);
}

} // namespace tisple_tests
