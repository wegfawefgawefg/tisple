// the 65C02 target (backend/cmos.hpp; the jump tables, dispatchers and interrupt entry): each
// program is built for both chips and runs like the interpreter on its own chip, the 65C02's
// instructions are where they should be, and its build never takes more cycles or bytes

#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

using tisple::Cpu;
using tisple::Mn;
using tisple::Mode;

Compiled BuildFor(const std::string& text, Cpu cpu) {
    tisple::BuildOptions options;
    options.cpu = cpu;
    return CompileTextWith(text, options);
}

int CountIns(const Compiled& c, const std::string& function, Mn mn, Mode mode) {
    int count = 0;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.name != function) {
            continue;
        }
        for (std::size_t i = span.first; i < span.end; i++) {
            const tisple::AsmItem& item = c.code.as.items[i];
            count += item.kind == tisple::ItemKind::Ins && item.mn == mn && item.mode == mode;
        }
    }
    return count;
}

std::uint32_t FunctionBytes(const Compiled& c, const std::string& function) {
    std::uint32_t bytes = 0;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        for (std::size_t i = span.first; i < span.end && span.name == function; i++) {
            bytes += c.code.as.items[i].size;
        }
    }
    return bytes;
}

// every argument on one chip's build: like the interpreter; the cycles of all the calls
std::uint64_t RunAll(TestResults& results, const Compiled& c, const std::string& function,
                     const std::vector<std::uint16_t>& args) {
    Diff d;
    std::uint64_t cycles = 0;
    bool ok = c.ok && StartDiff(d, c);
    for (std::size_t i = 0; ok && i < args.size(); i++) {
        ok = DiffCall(d, function, {args[i]});
        cycles += d.last_cycles;
    }
    CHECK(results, ok);
    if (!ok) {
        std::fprintf(stderr, "%s build: %s\n%s\n", tisple::CpuName(c.code.cpu),
                     c.ok ? d.message.c_str() : c.errors.c_str(), c.built.ok ? "" : "");
    }
    return cycles;
}

struct Case {
    const char* text;
    Mn mn;     // a 65C02 instruction its build must have
    Mode mode; // in this mode
};

const std::vector<std::uint16_t> kArgs = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 31, 127, 128, 200, 255};

void TestCases(TestResults& results) {
    const Case cases[] = {
        // a u16 cleared and an element zeroed: STZ, no LDA #0
        {"(let g u16 7)\n(let a (array u8 16))\n"
         "(fn f ((x u8)) u8 (set g 0) (set (at a (& x 15)) 0) (+ x (at a 3)))",
         Mn::STZ, Mode::AbsX},
        // a store through a pointer at its start: (zp)
        {"(let buf (array u8 256))\n"
         "(fn f ((x u8)) u8 (let p (ptr u8) (addr_of (at buf x))) (store p (+ x 5)) "
         "(at buf x))",
         Mn::STA, Mode::ZpInd},
        // an add of 1 whose carry nothing reads: INC A
        {"(let g u8 0)\n(fn f ((x u8)) u8 (set g (+ (* x 3) 1)) (+ g x))", Mn::INC, Mode::Acc},
        // bits set and cleared in a byte: TSB and TRB
        {"(let flags u8 0x41)\n(fn f ((x u8)) u8 (set flags (| flags 6)) (* x 3))", Mn::TSB,
         Mode::Abs},
        {"(let flags u8 0x47)\n(fn f ((x u8)) u8 (set flags (& flags 0xF9)) (* x 3))", Mn::TRB,
         Mode::Abs},
        // a dense match with code in its clauses: JMP (table,X)
        {"(let g u8 0)\n"
         "(fn f ((x u8)) u8\n"
         "    (match x (0 (set g 10)) (1 (set g (+ g 3))) (2 (set g (* g 2)))\n"
         "        (3 (set g (- g 1))) (4 (set g (^ g x))) (5 (set g (| g 1)))\n"
         "        (6 (set g 77)) (_ (set g (+ g x))))\n"
         "    g)",
         Mn::JMP, Mode::AbsIndX},
        // a call through a function pointer: JMP (pointer)
        {"(let g u8 0)\n"
         "(fn inc ((v u8)) u8 (+ v 1))\n(fn dbl ((v u8)) u8 (* v 2))\n"
         "(let fns (array (fn (u8) u8) 2))\n"
         "(fn f ((x u8)) u8 (set (at fns 0) inc) (set (at fns 1) dbl)\n"
         "    (let h (fn (u8) u8) (at fns (& x 1))) (h x))",
         Mn::JMP, Mode::Ind},
    };
    for (const Case& c : cases) {
        Compiled nmos = BuildFor(c.text, Cpu::k6502);
        Compiled cmos = BuildFor(c.text, Cpu::k65c02);
        std::uint64_t nmos_cycles = RunAll(results, nmos, "t.f", kArgs);
        std::uint64_t cmos_cycles = RunAll(results, cmos, "t.f", kArgs);
        bool uses = false;
        for (const tisple::FunctionSpan& span : cmos.code.spans) {
            uses = uses || CountIns(cmos, span.name, c.mn, c.mode) > 0;
        }
        CHECK(results, uses);
        CHECK(results, cmos_cycles < nmos_cycles);
        CHECK(results, FunctionBytes(cmos, "t.f") <= FunctionBytes(nmos, "t.f"));
        CHECK(results, CountIns(nmos, "t.f", Mn::STZ, Mode::Abs) == 0); // the 6502's stays NMOS
        if (!uses || cmos_cycles >= nmos_cycles) {
            std::fprintf(stderr, "65c02 case (%s %d): %llu cycles, the 6502's %llu\n%s\n",
                         tisple::MnName(c.mn), static_cast<int>(c.mode),
                         static_cast<unsigned long long>(cmos_cycles),
                         static_cast<unsigned long long>(nmos_cycles), c.text);
        }
    }
}

} // namespace

void RunBackendCmosTests(TestResults& results) {
    TestCases(results);
}

} // namespace tisple_tests
