// the examples on the 6502, the startup end to end, and a cycle report for a few kernels

#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

const char* const kExamples[] = {
    "examples/fill.tsp",    "examples/hires_plot.tsp", "examples/keys.tsp",
    "examples/macros.tsp",  "examples/read_key.tsp",   "examples/speaker.tsp",
    "examples/sprites.tsp", "examples/dot/main.tsp",   "examples/multi/main.tsp",
};

// bytes of one function's code
std::uint32_t FunctionBytes(const Compiled& c, const std::string& name) {
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.name != name) {
            continue;
        }
        std::uint32_t bytes = 0;
        for (std::size_t i = span.first; i < span.end; i++) {
            bytes += c.code.as.items[i].size;
        }
        return bytes;
    }
    return 0;
}

std::uint32_t RuntimeBytes(const Compiled& c) {
    return FunctionBytes(c, "runtime");
}

void TestExamplesCompile(TestResults& results) {
    for (const char* path : kExamples) {
        Compiled c = CompileFile(path);
        if (!c.ok) {
            std::fprintf(stderr, "%s: %s\n", path, c.errors.c_str());
        }
        CHECK(results, c.ok);
    }
}

void TestHgrInit(TestResults& results, std::uint64_t& cycles, std::uint32_t& bytes) {
    Compiled c = CompileFile("examples/dot/main.tsp");
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "gfx/hgr.init", {});
    cycles = d.last_cycles;
    bytes = FunctionBytes(c, "gfx/hgr.init");
    // the row tables in simulator memory are the hi-res row addresses
    const tisple::Program& program = c.built.program;
    std::int32_t lo = tisple::FindVar(program, "gfx/hgr.row_lo");
    std::int32_t hi = tisple::FindVar(program, "gfx/hgr.row_hi");
    const std::uint8_t* mem = d.sim.machine->mem;
    bool rows = true;
    for (std::uint32_t y = 0; y < 192; y++) {
        std::uint32_t want = 0x2000 + (y % 8) * 0x400 + ((y / 8) % 8) * 0x80 + (y / 64) * 0x28;
        std::uint32_t got =
            mem[c.code.layout.vars[static_cast<std::size_t>(lo)].address + y] |
            (mem[c.code.layout.vars[static_cast<std::size_t>(hi)].address + y] << 8);
        rows = rows && got == want;
    }
    CHECK(results, rows);
    // and the dot moves: flip draws and erases one pixel
    CheckDiff(results, d, "gfx/hgr.flip", {140, 96});
    CheckDiff(results, d, "gfx/hgr.flip", {279, 191});
    CheckDiff(results, d, "gfx/hgr.flip", {140, 96});
    CheckDiff(results, d, "main.read_action", {});
}

void TestFill(TestResults& results, std::uint64_t& cycles, std::uint32_t& bytes) {
    Compiled c = CompileFile("examples/fill.tsp");
    Diff d;
    CHECK(results, StartDiff(d, c));
    for (std::uint32_t a = 0x1F00; a < 0x4100; a++) {
        d.in.memory[a] = 0xAA;
        d.sim.machine->mem[a] = 0xAA;
    }
    CheckDiff(results, d, "fill.clear_hgr", {});
    cycles = d.last_cycles;
    bytes = FunctionBytes(c, "fill.fill") + FunctionBytes(c, "fill.clear_hgr");
    const std::uint8_t* mem = d.sim.machine->mem;
    bool cleared = true;
    for (std::uint32_t a = 0x2000; a < 0x4000; a++) {
        cleared = cleared && mem[a] == 0;
    }
    CHECK(results, cleared);
    CHECK(results, mem[0x1FFF] == 0xAA && mem[0x4000] == 0xAA);
}

void TestSprites(TestResults& results, std::uint64_t& cycles, std::uint32_t& bytes) {
    Compiled c = CompileFile("examples/sprites.tsp");
    Diff d;
    CHECK(results, StartDiff(d, c));
    for (std::uint16_t i = 0; i < 32; i++) {
        CheckDiff(results, d, "sprites.spawn",
                  {static_cast<std::uint16_t>(i * 7), static_cast<std::uint16_t>(i * 3),
                   static_cast<std::uint16_t>(i % 3 == 0 ? 0xFF : i % 5)});
    }
    CheckDiff(results, d, "sprites.spawn", {1, 1, 1});
    CheckDiff(results, d, "sprites.step_sprites", {});
    cycles = d.last_cycles;
    bytes = FunctionBytes(c, "sprites.step_sprites");
    CheckDiff(results, d, "sprites.step_sprites", {});
}

void TestOtherExamples(TestResults& results) {
    Compiled keys = CompileFile("examples/keys.tsp");
    Diff d;
    CHECK(results, StartDiff(d, keys));
    for (int i = 0; i < 40; i++) {
        CheckDiff(results, d, "keys.read_command", {});
        CheckDiff(results, d, "keys.run", {static_cast<std::uint16_t>(i % 5)});
    }
    Compiled macros = CompileFile("examples/macros.tsp");
    Diff m;
    CHECK(results, StartDiff(m, macros));
    CheckDiff(results, m, "macros.shuffle", {3, 10});
    CheckDiff(results, m, "macros.shuffle", {250, 10});
    Compiled speaker = CompileFile("examples/speaker.tsp");
    Diff s;
    CHECK(results, StartDiff(s, speaker));
    CheckDiff(results, s, "speaker.tone", {20, 5});
    Compiled plot = CompileFile("examples/hires_plot.tsp");
    Diff p;
    CHECK(results, StartDiff(p, plot));
    CheckDiff(results, p, "hires_plot.plot", {0, 0});
    CheckDiff(results, p, "hires_plot.plot", {279, 191});
    CheckDiff(results, p, "hires_plot.hline", {10, 200, 77});
    Compiled multi = CompileFile("examples/multi/main.tsp");
    Diff l;
    CHECK(results, StartDiff(l, multi));
    CheckDiff(results, l, "main.main", {});
}

// the binary as BRUN runs it: startup saves zero page, main runs, zero page comes back
void TestStartup(TestResults& results) {
    Compiled c = CompileText("(let total u16 1000)\n"
                             "(let pp (ptr u8))\n"
                             "(let bytes (array u8 8))\n"
                             "(fn main (returns u16)\n"
                             "    (set pp (addr_of (at bytes 0)))\n"
                             "    (for i u8 0 8 (store (+ pp (u16 i)) (* i 3)))\n"
                             "    (for i u8 0 8 (set total (+ total (u16 (at bytes i)))))\n"
                             "    total)");
    CHECK(results, c.ok);
    if (!c.ok) {
        std::fprintf(stderr, "%s\n", c.errors.c_str());
        return;
    }
    tisple::SimRunner run;
    tisple::SimMain result = tisple::RunCompiledMain(run, c.code, 1000000);
    CHECK(results, result.finished);
    CHECK(results, result.zp_restored);
    CHECK(results, (result.a | (result.x << 8)) == 1000 + 3 * 28);
    CHECK(results, c.code.image.size() == c.code.code_bytes + c.code.data_bytes);
    // the program owns zero page while main runs, so ptr lives there
    std::int32_t ptr = tisple::FindVar(c.built.program, "t.pp");
    CHECK(results, c.code.layout.vars[static_cast<std::size_t>(ptr)].address < 0x100);
    // --origin moves everything
    tisple::BuildOptions options;
    options.origin = 0x0803;
    tisple::Compiled low;
    std::vector<tisple::Diagnostic> errors;
    CHECK(results, tisple::CompileProgram(c.built.program, options, low, errors));
    tisple::SimRunner low_run;
    tisple::SimMain low_result = tisple::RunCompiledMain(low_run, low, 1000000);
    CHECK(results, low_result.finished && low_result.zp_restored);
    CHECK(results, (low_result.a | (low_result.x << 8)) == 1084);
}

void TestFrameOverlap(TestResults& results) {
    // a and b are never live together, so their frames share bytes; f calls both and keeps
    // its own values across the calls
    Compiled c = CompileText("(fn a (args (x u16) (y u16)) (returns u16) (let t (+ x y)) (* t 3))\n"
                             "(fn b (args (x u8)) (returns u8) (let t (+ x 1)) (* t t))\n"
                             "(fn f (args (n u16)) (returns u16)\n"
                             "    (let keep (+ n 5))\n"
                             "    (let r (a n keep))\n"
                             "    (+ (+ r (u16 (b (u8 n)))) keep))");
    CHECK(results, c.ok);
    const tisple::Program& program = c.built.program;
    auto address = [&](const char* fn, std::size_t param) {
        std::int32_t id = tisple::FindFunction(program, fn);
        std::int32_t var = program.functions[static_cast<std::size_t>(id)].params[param];
        return c.code.layout.vars[static_cast<std::size_t>(var)].address;
    };
    CHECK(results, address("t.a", 0) == address("t.b", 0));
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.f", {1000});
}

void TestErrors(TestResults& results) {
    Compiled big = CompileText("(let a (array u8 30000))\n(fn main (set (at a 5) 1))");
    CHECK(results, !big.ok && big.errors.find("no room") != std::string::npos);
}

void CycleReport(TestResults& results) {
    std::uint64_t fill = 0;
    std::uint64_t rows = 0;
    std::uint64_t sprites = 0;
    std::uint32_t fill_bytes = 0;
    std::uint32_t rows_bytes = 0;
    std::uint32_t sprite_bytes = 0;
    TestFill(results, fill, fill_bytes);
    TestHgrInit(results, rows, rows_bytes);
    TestSprites(results, sprites, sprite_bytes);
    Compiled mul = CompileText("(fn mul (args (a u16) (b u16)) (returns u16) (* a b))");
    Diff d;
    CHECK(results, StartDiff(d, mul));
    CheckDiff(results, d, "t.mul", {1234, 5678});
    std::uint64_t mul_cycles = d.last_cycles;
    CheckDiff(results, d, "t.mul", {0xFFFF, 0xFFFF});
    std::uint64_t mul_worst = d.last_cycles;
    std::uint32_t mul_bytes = FunctionBytes(mul, "t.mul") + RuntimeBytes(mul);
    std::printf("cycles: fill 8 KB %llu (%.1f a byte), %u bytes\n",
                static_cast<unsigned long long>(fill), static_cast<double>(fill) / 8192.0,
                fill_bytes);
    std::printf("cycles: hgr row tables %llu, %u bytes\n", static_cast<unsigned long long>(rows),
                rows_bytes);
    std::printf("cycles: sprite step over 32 %llu, %u bytes\n",
                static_cast<unsigned long long>(sprites), sprite_bytes);
    std::printf("cycles: 16-bit multiply 1234*5678 %llu, $FFFF*$FFFF %llu, %u bytes with the "
                "routine\n",
                static_cast<unsigned long long>(mul_cycles),
                static_cast<unsigned long long>(mul_worst), mul_bytes);
}

} // namespace

void RunBackendProgramTests(TestResults& results) {
    TestExamplesCompile(results);
    TestOtherExamples(results);
    TestStartup(results);
    TestFrameOverlap(results);
    TestErrors(results);
    CycleReport(results);
}

} // namespace tisple_tests
