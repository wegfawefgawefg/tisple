// (wait_cycles n) through the compiler: exact cycles for constant and variable counts at
// code addresses on every side of a page boundary, a constant and a variable n alike from
// kVarWaitMin on, the waits kept where they were written, the registers and memory around
// them as the interpreter has them, and the checker's errors

#include "asm/wait.hpp"
#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

Compiled CompileAt(const std::string& text, std::uint16_t origin) {
    Compiled c;
    c.built = BuildText(text);
    if (!c.built.ok) {
        c.errors = ErrorText(c.built);
        return c;
    }
    std::vector<tisple::Diagnostic> errors;
    tisple::BuildOptions options;
    options.origin = origin;
    options.keep_all_functions = true;
    c.ok = tisple::CompileProgram(c.built.program, options, c.code, errors);
    c.errors = tisple::FormatDiagnostics(c.built.program.sources, errors);
    return c;
}

// cycles of one call, entered by a jump and left by its RTS; -1 if it did not return
std::int64_t CallCycles(const Compiled& c, tisple::SimRunner& sim, const std::string& function,
                        const std::vector<std::uint16_t>& args) {
    std::int32_t id = tisple::FindFunction(c.built.program, function);
    tisple::SimCall call = tisple::CallCompiled(sim, c.built.program, c.code, id, args, 10000000);
    return call.returned ? static_cast<std::int64_t>(call.cycles) : -1;
}

std::uint16_t WaitAddress(const Compiled& c, const std::string& function) {
    for (const tisple::FunctionSpan& span : c.code.spans) {
        for (std::size_t i = span.first; i < span.end && span.name == function; i++) {
            if (c.code.as.items[i].kind == tisple::ItemKind::Wait) {
                return c.code.as.items[i].address;
            }
        }
    }
    return 0;
}

const char* const kTimed = "(let v u16 0)\n"
                           "(fn e () void)\n"
                           "(fn w2 () void (wait_cycles 2))\n"
                           "(fn w47 () void (wait_cycles 47))\n"
                           "(fn w1000 () void (wait_cycles 1000))\n"
                           "(fn w1290 () void (wait_cycles 1290))\n"
                           "(fn w65535 () void (wait_cycles 65535))\n"
                           "(const BASE u16 300)\n"
                           "(fn wk () void (wait_cycles (+ BASE 5)))\n"
                           "(fn wv () void (wait_cycles v))\n"
                           "(fn wp ((n u16)) void (wait_cycles n))\n";

// every wait at code addresses that put it at each byte of a page's last 48 and first 16
void TestTiming(TestResults& results) {
    struct Want {
        const char* function;
        std::uint32_t cycles;
    };
    const Want constant[] = {{"t.w2", 2},       {"t.w47", 47},       {"t.w1000", 1000},
                             {"t.w1290", 1290}, {"t.w65535", 65535}, {"t.wk", 305}};
    int failures = 0;
    for (std::uint16_t shift = 0; shift < 256; shift += 5) {
        Compiled c = CompileAt(kTimed, static_cast<std::uint16_t>(0x6000 + shift));
        tisple::SimRunner sim;
        std::string error;
        if (!c.ok || !tisple::StartCompiled(sim, c.code, error)) {
            std::fprintf(stderr, "%s%s\n", c.errors.c_str(), error.c_str());
            CHECK(results, false);
            return;
        }
        std::int64_t base = CallCycles(c, sim, "t.e", {});
        for (const Want& want : constant) {
            std::int64_t got = CallCycles(c, sim, want.function, {}) - base;
            if (got != want.cycles && failures++ < 10) {
                std::fprintf(stderr, "%s at $%04X: %lld cycles\n", want.function,
                             WaitAddress(c, want.function), static_cast<long long>(got));
            }
            CHECK(results, got == want.cycles);
        }
        const std::uint16_t counts[] = {0, 1, 2, 68, 69, 70, 255, 256, 1001, 40000, 65535};
        for (std::uint16_t n : counts) {
            std::int64_t want = n < tisple::kVarWaitMin ? tisple::kVarWaitMin : n;
            std::int64_t got = CallCycles(c, sim, "t.wp", {n}) - base;
            CHECK(results, got == want);
            std::int32_t v = tisple::FindVar(c.built.program, "t.v");
            std::uint16_t at = c.code.layout.vars[static_cast<std::size_t>(v)].address;
            sim.machine->mem[at] = static_cast<std::uint8_t>(n & 0xFF);
            sim.machine->mem[at + 1] = static_cast<std::uint8_t>(n >> 8);
            got = CallCycles(c, sim, "t.wv", {}) - base;
            CHECK(results, got == want);
        }
    }
}

// from kVarWaitMin on, a constant n and the same n read from a variable take the same cycles,
// so a value the optimiser folds into a constant waits as long as before
void TestSameAsConstant(TestResults& results) {
    const std::uint32_t counts[] = {tisple::kVarWaitMin,
                                    tisple::kVarWaitMin + 1,
                                    100,
                                    255,
                                    256,
                                    257,
                                    1285,
                                    1300,
                                    4097,
                                    40000,
                                    65534,
                                    65535};
    std::string text = "(fn e () void)\n(fn wp ((n u16)) void (wait_cycles n))\n";
    for (std::uint32_t n : counts) {
        text += "(fn c" + std::to_string(n) + " () void (wait_cycles " + std::to_string(n) + "))\n";
    }
    for (std::uint16_t shift = 0; shift < 256; shift += 17) {
        Compiled c = CompileAt(text, static_cast<std::uint16_t>(0x6000 + shift));
        tisple::SimRunner sim;
        std::string error;
        if (!c.ok || !tisple::StartCompiled(sim, c.code, error)) {
            std::fprintf(stderr, "%s%s\n", c.errors.c_str(), error.c_str());
            CHECK(results, false);
            return;
        }
        for (std::uint32_t n : counts) {
            std::int64_t constant = CallCycles(c, sim, "t.c" + std::to_string(n), {});
            std::int64_t variable = CallCycles(c, sim, "t.wp", {static_cast<std::uint16_t>(n)});
            if (constant != variable) {
                std::fprintf(stderr, "wait %u at origin $%04X: constant %lld, variable %lld\n", n,
                             0x6000 + shift, static_cast<long long>(constant),
                             static_cast<long long>(variable));
            }
            CHECK(results, constant == variable && constant >= 0);
        }
    }
}

int CountWaits(const Compiled& c, const std::string& function) {
    int count = 0;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        for (std::size_t i = span.first; i < span.end && span.name == function; i++) {
            count += c.code.as.items[i].kind == tisple::ItemKind::Wait ? 1 : 0;
        }
    }
    return count;
}

// nothing drops, merges or moves a wait, and what is around it comes out as the interpreter
// has it
void TestKept(TestResults& results) {
    const char* text = "(let out (array u8 16))\n"
                       "(let w (array u16 4) (make (array u16 4) 1 300 2 7))\n"
                       "(fn idle () void (for i u8 0 10 (wait_cycles 10)))\n"
                       "(fn pair () void (wait_cycles 5) (wait_cycles 5))\n"
                       "(fn tone () void\n"
                       "    (io_write u8 0xC030 1) (wait_cycles 20) (io_write u8 0xC030 2))\n"
                       "(fn f ((n u16)) u8\n"
                       "    (let s u8 0)\n"
                       "    (for i u8 0 16\n"
                       "        (wait_cycles 7)\n"
                       "        (set (at out i) (+ s i))\n"
                       "        (wait_cycles n)\n"
                       "        (wait_cycles (at w (& i 3)))\n"
                       "        (set s (+ s (at out i)))\n"
                       "        (wait_cycles 1300))\n"
                       "    s)\n";
    Compiled c = CompileText(text);
    CHECK(results, c.ok && CountWaits(c, "t.idle") >= 1 && CountWaits(c, "t.pair") == 2);
    CHECK(results, CountWaits(c, "t.tone") == 1 && CountWaits(c, "t.f") == 4);
    tisple::SimRunner sim;
    std::string error;
    CHECK(results, tisple::StartCompiled(sim, c.code, error));
    CHECK(results, CallCycles(c, sim, "t.idle", {}) >= 100);
    // the call comes in by a jump, so only the RTS is counted
    CHECK(results, CallCycles(c, sim, "t.pair", {}) == 10 + 6);
    const std::uint16_t counts[] = {0, 3, 200};
    for (std::uint16_t n : counts) {
        CheckProgram(results, text, "t.f", {n});
    }
    CheckProgram(results, text, "t.tone", {});
}

void TestErrors(TestResults& results) {
    ExpectError(results, "(fn main () void (wait_cycles 0))",
                "t.tsp:1:18: (wait_cycles 0) waits for nothing; remove it");
    ExpectError(results, "(fn main () void (wait_cycles 1))",
                "t.tsp:1:18: (wait_cycles 1): no 6502 instruction takes 1 cycle, so a wait is 2 "
                "or more; write (wait_cycles 2) or leave it out");
    ExpectError(results, "(const N u16 3)\n(fn main () void (wait_cycles (- N 2)))",
                "t.tsp:2:18: (wait_cycles (- N 2)): no 6502 instruction takes 1 cycle, so a "
                "wait is 2 or more; write (wait_cycles 2) or leave it out");
    ExpectError(results, "(fn main () void (wait_cycles))",
                "t.tsp:1:18: wait_cycles takes one count of cycles; write (wait_cycles 100)");
    ExpectError(results, "(fn main () void (wait_cycles 70000))",
                "t.tsp:1:31: 70000 does not fit in u16 (0 to 65535)");
    ExpectError(results, "(fn main ((b u8)) void (wait_cycles b))",
                "t.tsp:1:37: the wait_cycles count needs a u16 but b is u8; write (as u16 b)");
    ExpectError(results, "(fn wait_cycles () void)",
                "t.tsp:1:5: wait_cycles is a built-in name; rename this fn");
}

} // namespace

void RunBackendWaitTests(TestResults& results) {
    TestTiming(results);
    TestSameAsConstant(results);
    TestKept(results);
    TestErrors(results);
}

} // namespace tisple_tests
