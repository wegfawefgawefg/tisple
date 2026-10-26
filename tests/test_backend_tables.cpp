// an operation on two table reads with computed indices: the second index in X, the first
// read in A, then OP table,X (gen_arith.cpp IndexedSecond), and loop counters that move to
// memory to leave X free for it; on the 6502 against the interpreter

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

const char* const kTables =
    "(let g u8 0)\n"
    "(let lo (array u8 256))\n"
    "(let hi (array u8 256))\n"
    "(let small (array u8 16))\n"
    "(let out (array u8 40))\n"
    "(fn side () u8 (set g (+ g 3)) g)\n"
    "(fn fill () void\n"
    "    (for i u8 0 255 (set (at lo i) (* i 7)) (set (at hi i) (^ i 0x5A)))\n"
    "    (set (at lo 255) 1) (set (at hi 255) 2)\n"
    "    (for i u8 0 16 (set (at small i) (* i 13))))\n";

void CheckEveryByte(TestResults& results, const std::string& body) {
    std::string text = std::string(kTables) + body;
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c) && DiffCall(d, "t.fill", {});
    for (std::uint16_t v = 0; v < 256 && ok; v++) {
        ok = DiffCall(d, "t.f", {v, static_cast<std::uint16_t>((v * 91 + 7) & 0xFF)});
        if (!ok) {
            std::fprintf(stderr, "f %u: ", v);
        }
    }
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

bool HasOraAbsX(const Compiled& c, const std::string& function) {
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.name != function) {
            continue;
        }
        for (std::size_t i = span.first; i < span.end; i++) {
            const tisple::AsmItem& item = c.code.as.items[i];
            if (item.kind == tisple::ItemKind::Ins && item.mn == tisple::Mn::ORA &&
                item.mode == tisple::Mode::AbsX) {
                return true;
            }
        }
    }
    return false;
}

void TestTwoReads(TestResults& results) {
    const char* bodies[] = {
        "(fn f ((x u8) (y u8)) u8 (| (at lo (+ x y)) (at hi (- x y))))",
        "(fn f ((x u8) (y u8)) u8 (- (at lo (+ x 1)) (at hi (+ y x))))",
        "(fn f ((x u8) (y u8)) u8 (+ (at lo (<< x 1)) (at hi (+ y 3))))",
        "(fn f ((x u8) (y u8)) u8 (^ (+ x (at lo (+ x y))) (at hi (& (+ x y) 0x7F))))",
        // the first read's index is itself a table read
        "(fn f ((x u8) (y u8)) u8 (& (at lo (at hi (+ x 1))) (at hi (+ (at lo y) 1))))",
        "(fn f ((x u8) (y u8)) u8 (| (at small (& (+ x y) 15)) (at lo (+ (at small (& x 15)) "
        "y))))",
        // a call in either side keeps the order: side changes g, which the other side reads
        "(fn f ((x u8) (y u8)) u8 (+ (at lo (+ (side) x)) (at hi (+ g y))))",
        "(fn f ((x u8) (y u8)) u8 (- (at lo (+ g x)) (at hi (+ (side) y))))",
        // the second is first in the source: commuted, the other way round
        "(fn f ((x u8) (y u8)) u8 (| (at hi (+ x y)) (+ (at lo (+ y 2)) x)))",
        // in a loop whose counter is in memory to leave X free
        "(fn f ((x u8) (y u8)) u8\n"
        "    (for i u8 0 40 (set (at out i) (| (at lo (+ (at out i) x)) (at hi (+ i y)))))\n"
        "    (at out (& x 31)))",
        "(fn f ((x u8) (y u8)) u8\n"
        "    (for r u8 0 3\n"
        "        (let a u8 (at lo (+ r x)))\n"
        "        (set (at out 0) (| (at lo (+ (at small 1) a)) (at hi (+ (at small 1) y))))\n"
        "        (set (at out 1) (| (at lo (+ (at small 2) a)) (at hi (+ (at small 2) y)))))\n"
        "    (+ (at out 0) (at out 1)))",
    };
    for (const char* body : bodies) {
        CheckEveryByte(results, body);
    }
    Compiled c =
        CompileText(std::string(kTables) +
                    "(fn f ((x u8) (y u8)) u8 (| (at lo (+ x y)) (at hi (- x y))))\n"
                    "(fn h ((x u8) (y u8)) void\n"
                    "    (for r u8 0 3\n"
                    "        (let a u8 (at lo (+ r x)))\n"
                    "        (set (at out 0) (| (at lo (+ (at small 1) a)) (at hi (+ (at small 1) "
                    "y))))\n"
                    "        (set (at out 1) (| (at lo (+ (at small 2) a)) (at hi (+ (at small 2) "
                    "y))))\n"
                    "        (set (at out 2) (| (at lo (+ (at small 3) a)) (at hi (+ (at small 3) "
                    "y))))))");
    CHECK(results, HasOraAbsX(c, "t.f"));
}

} // namespace

void RunBackendTableTests(TestResults& results) {
    TestTwoReads(results);
}

} // namespace tisple_tests
