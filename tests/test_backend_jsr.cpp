// (jsr addr (a v) (x v) (y v)): the code it makes, machine code run on the 6502 with the
// registers it was given, the optimiser keeping every call in place, and the checker's and
// the interpreter's answers

#include "fuzz_gen.hpp"
#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

// a function's instructions as "LDA #$01; JSR $0310; ..."
std::string Code(const Compiled& c, const std::string& function) {
    std::string text;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.name != function) {
            continue;
        }
        for (std::size_t i = span.first; i < span.end; i++) {
            const tisple::AsmItem& item = c.code.as.items[i];
            if (item.kind != tisple::ItemKind::Ins) {
                continue;
            }
            text += (text.empty() ? "" : "; ") + tisple::ItemText(c.code.as, item);
        }
    }
    return text;
}

void ExpectCode(TestResults& results, const Compiled& c, const std::string& function,
                const std::string& want) {
    std::string got = Code(c, function);
    if (!c.ok || got != want) {
        std::fprintf(stderr, "%s: want  %s\n%s: got   %s %s\n", function.c_str(), want.c_str(),
                     function.c_str(), got.c_str(), c.errors.c_str());
    }
    CHECK(results, c.ok && got == want);
}

// machine code for the tests, at $0300 in the simulator:
// $0300: STA $0340; STX $0341; STY $0342; INC $0343; LDX #$77; LDY #$77; LDA $0343; RTS
//        keeps the registers it got, counts its calls, clobbers X and Y, returns the count
// $0320: CLC; ADC #1; LDX #$55; LDY #$66; RTS   gives A + 1
const std::uint8_t kRoutines[] = {0x8D, 0x40, 0x03, 0x8E, 0x41, 0x03, 0x8C, 0x42, 0x03, 0xEE,
                                  0x43, 0x03, 0xA2, 0x77, 0xA0, 0x77, 0xAD, 0x43, 0x03, 0x60,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x18, 0x69, 0x01, 0xA2, 0x55, 0xA0, 0x66, 0x60};

struct JsrRun {
    Compiled c;
    tisple::SimRunner sim;
    bool ok = false;
};

bool StartJsr(JsrRun& run, const std::string& text) {
    run.c = CompileText(text);
    std::string error;
    if (!run.c.ok || !tisple::StartCompiled(run.sim, run.c.code, error)) {
        std::fprintf(stderr, "program:\n%s\n%s%s\n", text.c_str(), run.c.errors.c_str(),
                     error.c_str());
        return false;
    }
    for (std::size_t i = 0; i < sizeof(kRoutines); i++) {
        run.sim.machine->mem[0x0300 + i] = kRoutines[i];
    }
    run.ok = true;
    return true;
}

// calls a function; false if it did not return
bool CallJsr(JsrRun& run, const std::string& function, const std::vector<std::uint16_t>& args,
             std::uint8_t& a) {
    const tisple::Program& program = run.c.built.program;
    std::int32_t id = tisple::FindFunction(program, function);
    if (!run.ok || id < 0) {
        return false;
    }
    tisple::SimCall call = tisple::CallCompiled(run.sim, program, run.c.code, id, args, 1000000);
    a = call.a;
    return call.returned;
}

std::uint8_t Mem(const JsrRun& run, std::uint16_t address) {
    return run.sim.machine->mem[address];
}

std::uint8_t Global(const JsrRun& run, const std::string& name, std::uint32_t offset) {
    std::int32_t var = tisple::FindVar(run.c.built.program, name);
    if (var < 0) {
        return 0;
    }
    std::uint16_t at = static_cast<std::uint16_t>(
        run.c.code.layout.vars[static_cast<std::size_t>(var)].address + offset);
    return Mem(run, at);
}

void TestJsrCode(TestResults& results) {
    // a constant address: the registers, then a plain JSR
    Compiled c = CompileText("(fn f () u8 (jsr 0x0310 (a 1) (x 0xC4) (y 0x40)))");
    // (a JSR just before the RTS becomes a JMP, as any tail call does)
    ExpectCode(results, c, "t.f", "LDA #$01; LDX #$C4; LDY #$40; JMP $0310");
    // no registers; the value is dropped but the call stays
    c = CompileText("(fn f () u8 (jsr 0x0300) (jsr 0x0300) (jsr 0x0300 (a 1)) 5)");
    ExpectCode(results, c, "t.f", "JSR $0300; JSR $0300; LDA #$01; JSR $0300; LDA #$05; RTS");
    // a computed address is stored into the JSR's operand
    c = CompileText("(let r u16 0x0320)\n(fn f () u8 (jsr r (a 7)))");
    CHECK(results, c.ok && CountMn(c, "t.f", tisple::Mn::JMP) == 1);
    CHECK(results, c.ok && CountMn(c, "t.f", tisple::Mn::STA) == 2);
}

void TestJsrRuns(TestResults& results) {
    // the registers arrive as given, through a constant and a computed address
    JsrRun run;
    std::uint8_t a = 0;
    if (StartJsr(run, "(let r u16 0x0300) (let g u8 9)\n"
                      "(fn f ((v u8)) u8 (jsr 0x0300 (a v) (x (+ v 1)) (y g)))\n"
                      "(fn h ((v u8)) u8 (jsr r (x v) (y (* v 2))))\n"
                      "(fn k ((v u8)) u8 (jsr (+ r 0x20) (a v)))")) {
        CHECK(results, CallJsr(run, "t.f", {5}, a) && a == 1);
        CHECK(results, Mem(run, 0x0340) == 5 && Mem(run, 0x0341) == 6 && Mem(run, 0x0342) == 9);
        CHECK(results, CallJsr(run, "t.h", {20}, a) && a == 2);
        CHECK(results, Mem(run, 0x0341) == 20 && Mem(run, 0x0342) == 40);
        CHECK(results, CallJsr(run, "t.k", {41}, a) && a == 42);
    }
}

void TestJsrStaysPut(TestResults& results) {
    // reads of memory the routine changes stay on their own side of the call: the counter at
    // $0343 is declared, so it is a global the call may change
    JsrRun run;
    std::uint8_t a = 0;
    if (StartJsr(run, "(let n u8 0 :address 0x0343)\n"
                      "(fn f () u8\n"
                      "    (let before u8 n)\n"
                      "    (jsr 0x0300)\n"
                      "    (let after u8 n)\n"
                      "    (+ (* before 16) after))\n"
                      "(fn twice () u8 (+ (jsr 0x0300) (jsr 0x0300)))\n"
                      "(fn unused () void (let v u8 (jsr 0x0300)))")) {
        CHECK(results, CallJsr(run, "t.f", {}, a) && a == 0x01);
        CHECK(results, CallJsr(run, "t.f", {}, a) && a == 0x12);
        // never merged: 3 + 4
        CHECK(results, CallJsr(run, "t.twice", {}, a) && a == 7);
        // never dropped, though nothing reads its value
        CHECK(results, CallJsr(run, "t.unused", {}, a) && Mem(run, 0x0343) == 5);
    }
    // X and Y are reloaded after the call, which clobbers them
    JsrRun regs;
    if (StartJsr(regs, "(let arr (array u8 8))\n"
                       "(fn f ((i u8)) void\n"
                       "    (set (at arr i) 1)\n"
                       "    (jsr 0x0300 (x 0) (y 0))\n"
                       "    (set (at arr i) (+ (at arr i) 2)))\n"
                       "(fn g () void\n"
                       "    (for i u8 0 8\n"
                       "        (set (at arr i) (jsr 0x0320 (a (* i 3))))))")) {
        CHECK(results, CallJsr(regs, "t.f", {3}, a) && Global(regs, "t.arr", 3) == 3);
        CHECK(results, CallJsr(regs, "t.g", {}, a));
        bool all = true;
        for (std::uint32_t i = 0; i < 8; i++) {
            all = all && Global(regs, "t.arr", i) == i * 3 + 1;
        }
        CHECK(results, all);
    }
}

void TestJsrChecks(TestResults& results) {
    const char* shape = "write (jsr addr (a v) (x v) (y v)), each register at most once, in "
                        "that order";
    ExpectError(results, "(fn f () void (jsr 0x300 (x 1) (a 2)))",
                std::string("t.tsp:1:32: (a 2) is out of order or repeated; ") + shape);
    ExpectError(results, "(fn f () void (jsr 0x300 (a 1) (a 2)))",
                std::string("t.tsp:1:32: (a 2) is out of order or repeated; ") + shape);
    ExpectError(results, "(fn f () void (jsr 0x300 (p 1)))",
                std::string("t.tsp:1:26: (p 1) is not a register; ") + shape);
    ExpectError(results, "(fn f () void (jsr))",
                std::string("t.tsp:1:15: jsr takes an address and up to three registers; ") +
                    shape);
    ExpectError(results, "(fn f () void (jsr 0x300 (a 300)))",
                "t.tsp:1:29: 300 does not fit in u8 (0 to 255)");
    ExpectError(results, "(fn f ((p u8)) void (jsr p))",
                "t.tsp:1:26: the jsr address needs a u16 but p is u8; write (as u16 p)");
    // jsr is a built-in name: no function may take it
    ExpectError(results, "(fn jsr () void)", "t.tsp:1:5: jsr is a built-in name; rename this fn");
}

void TestJsrInterp(TestResults& results) {
    Built built = BuildText("(fn f () u8 (+ (jsr 0x0300) 1))");
    CHECK(results, built.ok);
    if (!built.ok) {
        return;
    }
    tisple::Interp in;
    StartInterp(in, built);
    tisple::InterpResult r =
        tisple::CallFunction(in, tisple::FindFunction(built.program, "t.f"), {});
    CHECK(results,
          r.kind == tisple::StopKind::Error && r.message.find("jsr needs the machine") == 0);
    // the fuzz generator never writes one: the interpreter is its oracle
    bool none = true;
    for (std::uint64_t seed = 0; seed < 500 && none; seed++) {
        none = GenerateProgram(seed).find("(jsr") == std::string::npos;
    }
    CHECK(results, none);
}

} // namespace

void RunBackendJsrTests(TestResults& results) {
    TestJsrCode(results);
    TestJsrRuns(results);
    TestJsrStaysPut(results);
    TestJsrChecks(results);
    TestJsrInterp(results);
}

} // namespace tisple_tests
