// constant folding before code generation (src/backend/fold.cpp), on the 6502 against the
// interpreter, which runs the program as written: chains of adds, multiplies, bitwise ops
// and pointer steps with constants in them, in every integer type, across wrapping

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

const char* const kGlobals = "(let g u8 0)\n"
                             "(let bytes (array u8 16) (make (array u8 16) 1 2 3 4 5 6 7 8 9 10 "
                             "11 12 13 14 15 16))\n"
                             "(let words (array u16 8) (make (array u16 8) 100 200 300 400 500 "
                             "600 700 800))\n"
                             "(fn side () u8 (set g (+ g 1)) g)\n";

// f called with every byte value as its first argument (a second one gets another byte)
void CheckEveryByte(TestResults& results, const std::string& body) {
    std::string text = std::string(kGlobals) + body;
    Compiled c = CompileText(text);
    Diff d;
    if (!StartDiff(d, c)) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
        CHECK(results, false);
        return;
    }
    std::int32_t f = tisple::FindFunction(c.built.program, "t.f");
    std::size_t params = c.built.program.functions[static_cast<std::size_t>(f)].params.size();
    bool ok = true;
    for (std::uint16_t v = 0; v < 256 && ok; v++) {
        std::vector<std::uint16_t> args = {v};
        if (params == 2) {
            args.push_back(static_cast<std::uint16_t>((v * 37 + 11) & 0xFF));
        }
        ok = DiffCall(d, "t.f", args);
        if (!ok) {
            std::fprintf(stderr, "program:\n%s\n  f %u: %s\n", text.c_str(), v, d.message.c_str());
        }
    }
    CHECK(results, ok);
}

void TestAddChains(TestResults& results) {
    const char* bodies[] = {
        "(fn f ((t u8)) u8 (+ (+ 5 t) 3))",
        "(fn f ((t u8)) u8 (- (+ t 7) 2))",
        "(fn f ((t u8)) u8 (- 3 (+ t 1)))",
        "(fn f ((t u8)) u8 (+ (- 5 t) 3))",
        "(fn f ((t u8)) u8 (- (- t 1) 255))",
        "(fn f ((t u8)) u8 (+ (+ t 200) 100))",
        "(fn f ((t u8)) u8 (+ (+ t 0) 0))",
        "(fn f ((t u8)) u8 (+ t (* 1 3)))",
        "(fn f ((t u8) (u u8)) u8 (+ (+ (- 9 t) u) (- 1 (+ 4 t))))",
        "(fn f ((t u8) (u u8)) u8 (- (+ 1 (side)) (+ (side) 1)))",
        "(fn f ((t i8)) i8 (+ (+ t 100) 100))",
        "(fn f ((t i8)) i8 (- (- t -128) 1))",
        "(fn f ((t i8)) bool (< (+ (+ t 100) 0) 3))",
        "(fn f ((t u8)) u16 (+ (as u16 (+ t 200)) 100))",
        "(fn f ((t u8)) u16 (+ (+ (as u16 t) 0xFFFF) 2))",
        "(fn f ((t u8)) u16 (- (+ (* (as u16 t) 300) 0x8000) 0x7FFF))",
        "(fn f ((t u8)) i16 (+ (+ (as i16 t) -30000) -30000))",
        "(fn f ((t u8)) u8 (at bytes (& (+ (+ t 1) 1) 15)))",
        "(fn f ((t u8)) u16 (at words (& (- (+ t 9) 1) 7)))",
    };
    for (const char* body : bodies) {
        CheckEveryByte(results, body);
    }
    // one add, no CLC pair: (+ (+ 5 t) 3) is (+ t 8)
    Compiled c = CompileText("(fn f ((t u8)) u8 (+ (+ 5 t) 3))");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::ADC) == 1);
    // (+ w 2) in place: one ADC, the high byte stepped on a carry (BCC / INC w+1)
    Compiled w = CompileText("(let w u16 0)\n(fn f () void (set w (+ (+ w 1) 1)))");
    CHECK(results, CountMn(w, "t.f", tisple::Mn::ADC) == 1);
    CHECK(results, CountMn(w, "t.f", tisple::Mn::INC) == 1);
}

void TestOtherChains(TestResults& results) {
    const char* bodies[] = {
        "(fn f ((t u8)) u8 (* (* t 3) 5))",
        "(fn f ((t u8)) u8 (* 1 (* t 1)))",
        "(fn f ((t u8)) u16 (* (* (as u16 t) 300) 300))",
        "(fn f ((t u8)) u8 (+ (* (side) 0) g))",
        "(fn f ((t u8)) u8 (+ (* t 0) g))",
        "(fn f ((t u8)) u8 (& (& t 0xF0) 0x3C))",
        "(fn f ((t u8)) u8 (& (& t 0xFF) (side)))",
        "(fn f ((t u8)) u8 (| (| t 1) 0xFF))",
        "(fn f ((t u8)) u8 (+ (| (side) 0xFF) g))",
        "(fn f ((t u8)) u8 (+ (& (side) 0) g))",
        "(fn f ((t u8)) u8 (^ (^ t 0x55) 0x55))",
        "(fn f ((t u8)) u8 (^ (^ 0x0F t) (^ 0xF0 (side))))",
        "(fn f ((t u8)) u8 (<< t 0))",
        "(fn f ((t u8)) u8 (>> (+ t 0) 0))",
        "(fn f ((t i8)) i8 (* (* t -1) -1))",
        "(fn f ((t i8)) i8 (>> (^ t 0) 0))",
        "(fn f ((t u8)) u16 (& (+ (as u16 t) 0x1FF) 0xFF00))",
        "(fn f ((t u8)) u8 (as u8 (+ (* (as u16 t) 1) (as u16 (+ t 0)))))",
    };
    for (const char* body : bodies) {
        CheckEveryByte(results, body);
    }
    Compiled c = CompileText(std::string(kGlobals) + "(fn f ((t u8)) u8 (+ (* (side) 0) g))");
    CHECK(results, CountMn(c, "t.f", tisple::Mn::JSR) + CountMn(c, "t.f", tisple::Mn::INC) > 0);
}

void TestPointerChains(TestResults& results) {
    const char* bodies[] = {
        "(fn f ((t u8)) u8 (load (+ (+ (addr_of (at bytes 0)) 1) 2)))",
        "(fn f ((t u8)) u8 (load (- (+ (addr_of (at bytes 0)) 5) 3)))",
        "(fn f ((t u8)) u8 (load (+ (+ (addr_of (at bytes 0)) 0) 0)))",
        "(fn f ((t u8)) u8 (load (+ (+ (+ (addr_of (at bytes 0)) (& t 3)) 2) 1)))",
        "(fn f ((t u8)) u8 (load (- (+ (+ (addr_of (at bytes 8)) 1) (& t 3)) 4)))",
        "(fn f ((t u8)) u16 (load (+ (+ (addr_of (at words 0)) 1) 2)))",
        "(fn f ((t u8)) u16 (load (- (+ (addr_of (at words 0)) 7) (+ 1 2))))",
        "(fn f ((t u8)) void (store (+ (+ (addr_of (at bytes 0)) 3) (& t 7)) t))",
        "(fn f ((t u8)) void (store (+ (+ (addr_of (at words 0)) 0) (& t 3)) (as u16 t)))",
        "(fn f ((t u8)) void\n"
        "    (let p (ptr u8) (addr_of (at bytes 0)))\n"
        "    (store (+ (+ p 1) 2) t) (store (+ (+ p 0) 4) (+ (load (+ (+ p 1) 1)) 1)))",
    };
    for (const char* body : bodies) {
        CheckEveryByte(results, body);
    }
}

// what plasma's unroll leaves: constant indices and constant parts in every copy
void TestUnrolledCopies(TestResults& results) {
    const char* macros = "(macro unroll (name start end & body)\n"
                         "    (let copies (list))\n"
                         "    (for i start end\n"
                         "        (set copies (append copies (list `(do ,@(rep body name i))))))\n"
                         "    `(do ,@copies))\n"
                         "(macro rep (form old new)\n"
                         "    (if (= form old) (return new))\n"
                         "    (if (!= (kind form) 'list) (return form))\n"
                         "    (let out (list))\n"
                         "    (for i 0 (len form) (set out (append out (list (rep (nth form i) "
                         "old new)))))\n"
                         "    out)\n";
    std::string body = std::string(macros) +
                       "(fn f ((t u8)) void\n"
                       "    (let p (ptr u8) (addr_of (at bytes 0)))\n"
                       "    (unroll x 0 4\n"
                       "        (set (at bytes (* x 3)) (+ (+ 5 t) x))\n"
                       "        (set (at bytes (+ (* x 2) 1)) (+ t (* x 3)))\n"
                       "        (store (+ (+ p x) 8) (- (+ t 7) x))\n"
                       "        (set g (+ (+ g x) 1))))";
    CheckEveryByte(results, body);
}

} // namespace

void RunBackendFoldTests(TestResults& results) {
    TestAddChains(results);
    TestOtherChains(results);
    TestPointerChains(results);
    TestUnrolledCopies(results);
}

} // namespace tisple_tests
