// generative differential fuzzing: random programs on the 6502 against the interpreter.
// TISPLE_FUZZ=N runs N programs (default 300), TISPLE_FUZZ_SEED picks the first seed,
// TISPLE_FUZZ_CYCLES=1 prints each program's cycles (to compare two backends seed by seed).
// A seed in four also runs a second program, with loops entered by a jump ("(entries)"), one
// in four a third, with dead work for the mid-level passes ("(dead)"), one in four a
// fourth, with loop-invariant work and constants ("(motion)"), one in four a fifth, with
// work done twice, branches on equalities and calls with constant arguments ("(common)"), one
// in four a sixth, with loops that collapse and compares ranges decide ("(collapse)"), and one
// in four a seventh, with steps, sets and elements the round's ranges decide ("(values)"), and
// one in four an eighth, with calls through function pointers ("(fnptr)"), and one in four a
// ninth, with an interrupt handler of its own, run with interrupts raised early in main
// ("(irq)"), and one in four a tenth, with calls to inline and divisions to work out
// ("(inline)"), and one in four an eleventh, with copies the size budget weighs, folds around
// calls and constant indexes ("(size)"), and one in four a twelfth, with I/O through bases in
// globals and helpers that hand their parameters on ("(io)", also at -O0: "(io -O0)").

#include "fuzz_gen.hpp"
#include "source_file.hpp"
#include "test_backend.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace tisple_tests {

namespace {

constexpr std::uint64_t kDefaultPrograms = 300;

std::uint64_t EnvNumber(const char* name, std::uint64_t fallback) {
    const char* text = std::getenv(name);
    if (!text || !*text) {
        return fallback;
    }
    return std::strtoull(text, nullptr, 0);
}

// the interrupts an (irq) program runs with: early in main on both, from the seed; half
// straight to the entry ($FFFE), half through a rom-like STA $45 / JMP ($03FE)
struct IrqPlan {
    bool on = false;
    std::vector<std::uint64_t> cycles;
    std::vector<std::uint64_t> steps;
    bool rom = false;
};

IrqPlan MakeIrqPlan(std::uint64_t seed) {
    IrqPlan plan;
    plan.on = true;
    std::uint64_t s = seed * 0x9E3779B97F4A7C15ull + 77;
    auto next = [&](std::uint64_t n) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (s >> 11) % n;
    };
    int count = 1 + static_cast<int>(next(8));
    std::uint64_t cycle = 20;
    std::uint64_t step = 5;
    for (int i = 0; i < count; i++) {
        cycle += 1 + next(400);
        step += 1 + next(40);
        plan.cycles.push_back(cycle);
        plan.steps.push_back(step);
    }
    plan.rom = next(2) == 0;
    return plan;
}

void StartIrqs(Diff& d, const IrqPlan& plan) {
    tisple::Machine& m = *d.sim.machine;
    std::uint16_t vector = d.c->code.irq_direct;
    if (plan.rom) {
        const std::uint8_t kRomLike[] = {0x85, 0x45, 0x6C, 0xFE, 0x03};
        for (std::size_t i = 0; i < sizeof(kRomLike); i++) {
            m.mem[0xFF00 + i] = kRomLike[i];
        }
        vector = 0xFF00;
    }
    m.mem[0xFFFE] = static_cast<std::uint8_t>(vector & 0xFF);
    m.mem[0xFFFF] = static_cast<std::uint8_t>(vector >> 8);
    for (std::uint64_t k : plan.cycles) {
        tisple::RequestIrq(m, m.cycles + k);
    }
    d.in.irq_steps = plan.steps;
}

struct FuzzStats {
    std::uint64_t programs = 0;
    std::uint64_t ran = 0;
    std::uint64_t skipped = 0; // the interpreter hit its step limit
    std::uint64_t invalid = 0; // the generator wrote something the checker rejects
    std::uint64_t unsupported = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t cycles = 0;
};

// every failing seed gets a line; the first three get their program too. `variant` is "" for
// the seed's program, " (entries)", " (dead)", " (motion)" or " (common)" for the others
void Report(const char* what, std::uint64_t seed, const char* variant, const std::string& text,
            const std::string& why, std::uint64_t count) {
    std::fprintf(stderr, "fuzz %s, seed %llu%s: %s\n", what, static_cast<unsigned long long>(seed),
                 variant, why.c_str());
    if (count <= 3) {
        std::fprintf(stderr, "%s\n", text.c_str());
    }
}

void FuzzOne(std::uint64_t seed, const char* variant, const std::string& text, FuzzStats& stats,
             const IrqPlan& irqs = IrqPlan(), const tisple::OptOptions& opt = {}) {
    stats.programs++;
    // TISPLE_FUZZ_PRINT=variant: print the programs of that variant ((fnptr), or "" for all)
    if (const char* print = std::getenv("TISPLE_FUZZ_PRINT")) {
        if (std::string(variant).find(print) != std::string::npos) {
            std::printf("; seed %llu%s\n%s\n", static_cast<unsigned long long>(seed), variant,
                        text.c_str());
        }
    }
    Compiled c = CompileMainText(text, opt);
    if (!c.built.ok) {
        stats.invalid++;
        Report("program does not check", seed, variant, text, c.errors, stats.invalid);
        return;
    }
    if (!c.ok) {
        stats.unsupported++;
        Report("program does not compile", seed, variant, text, c.errors, stats.unsupported);
        return;
    }
    Diff d;
    d.in.max_steps = 300000;
    d.max_cycles = 40000000;
    if (!StartDiff(d, c)) {
        stats.mismatches++;
        Report("startup", seed, variant, text, d.message, stats.mismatches);
        return;
    }
    if (irqs.on) {
        StartIrqs(d, irqs);
    }
    bool same = DiffCall(d, "t.main", {});
    // main ended before every interrupt came, on one side: nothing to compare
    if (irqs.on && d.sim.machine->irqs_taken != d.in.irqs_taken) {
        stats.skipped++;
        return;
    }
    if (same) {
        stats.ran++;
        stats.cycles += d.last_cycles;
        if (std::getenv("TISPLE_FUZZ_CYCLES")) {
            std::printf("seed %llu%s: %llu cycles\n", static_cast<unsigned long long>(seed),
                        variant, static_cast<unsigned long long>(d.last_cycles));
        }
        return;
    }
    if (d.message.rfind("interpreter stopped", 0) == 0) {
        stats.skipped++;
        return;
    }
    stats.mismatches++;
    Report("mismatch", seed, variant, text, d.message, stats.mismatches);
}

// TISPLE_FUZZ_FILE=path: run that one program's main both ways and say how they differ
// (TISPLE_FUZZ_OPT adds pass options)
bool DiffFile(TestResults& results) {
    const char* path = std::getenv("TISPLE_FUZZ_FILE");
    if (!path || !*path) {
        return false;
    }
    tisple::SourceFile file;
    std::string error;
    if (!tisple::LoadSourceFile(path, file, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        CHECK(results, false);
        return true;
    }
    // TISPLE_FUZZ_OPT="--opt-bisect 12" (or -O0, --no-pass NAME): those pass options
    tisple::OptOptions opt;
    std::vector<std::string> words;
    if (const char* flags = std::getenv("TISPLE_FUZZ_OPT")) {
        std::string word;
        for (const char* c = flags;; c++) {
            if (*c == ' ' || *c == 0) {
                if (!word.empty()) {
                    words.push_back(word);
                }
                word.clear();
                if (*c == 0) {
                    break;
                }
            } else {
                word += *c;
            }
        }
    }
    std::vector<char*> argv;
    for (std::string& word : words) {
        argv.push_back(word.data());
    }
    for (int i = 0; i < static_cast<int>(argv.size()); i++) {
        if (!tisple::ParseOptFlag(static_cast<int>(argv.size()), argv.data(), i, opt, error)) {
            std::fprintf(stderr, "TISPLE_FUZZ_OPT: %s\n", error.empty() ? argv[i] : error.c_str());
            CHECK(results, false);
            return true;
        }
    }
    Compiled c = CompileMainText(file.text, opt);
    Diff d;
    d.in.max_steps = 300000;
    d.max_cycles = 40000000;
    // TISPLE_FUZZ_IRQ=seed: the interrupts that seed's (irq) program runs with
    std::uint64_t irq_seed = EnvNumber("TISPLE_FUZZ_IRQ", 0);
    bool ok = StartDiff(d, c);
    if (ok && irq_seed != 0) {
        StartIrqs(d, MakeIrqPlan(irq_seed));
    }
    ok = ok && DiffCall(d, "t.main", {});
    if (ok && irq_seed != 0 && d.sim.machine->irqs_taken != d.in.irqs_taken) {
        d.message = std::to_string(d.sim.machine->irqs_taken) + " interrupts on the 6502, " +
                    std::to_string(d.in.irqs_taken) + " in the interpreter";
        ok = false;
    }
    if (opt.bisect >= 0) {
        std::printf("%s\n", tisple::DescribeOptReport(c.code.opt).c_str());
    }
    std::printf("%s: %s\n", path, ok ? "same" : d.message.c_str());
    if (c.ok) {
        std::printf("%s", tisple::ListProgram(c.built.program, c.code).c_str());
    }
    CHECK(results, ok);
    return true;
}

// the programs of fuzz farm miscompiles, as the generator wrote them then (seed.tsp, and
// run6/ for the 227 of the sixth run), so they stay covered whatever the generator writes for
// those seeds now
void RunFarmPrograms(TestResults& results) {
    std::string dir = std::string(TISPLE_SOURCE_DIR) + "/tests/programs/fuzz_farm";
    std::vector<std::string> paths;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.path().extension() == ".tsp") {
            paths.push_back(entry.path().string());
        }
    }
    std::sort(paths.begin(), paths.end());
    for (const std::string& path : paths) {
        tisple::SourceFile file;
        std::string error;
        CHECK(results, tisple::LoadSourceFile(path, file, error));
        // and with no mid-level passes, which fold away some of what the backend got wrong
        for (int level : {1, 0}) {
            tisple::OptOptions opt;
            opt.level = level;
            Compiled c = CompileMainText(file.text, opt);
            Diff d;
            d.in.max_steps = 300000;
            d.max_cycles = 40000000;
            bool ok = StartDiff(d, c) && DiffCall(d, "t.main", {});
            if (!ok) {
                std::fprintf(stderr, "fuzz farm program %s at -O%d: %s\n", path.c_str(), level,
                             d.message.c_str());
            }
            CHECK(results, ok);
        }
    }
    CHECK(results, paths.size() >= 14 + 227);
}

} // namespace

void RunBackendFuzzTests(TestResults& results) {
    if (DiffFile(results)) {
        return;
    }
    RunFarmPrograms(results);
    std::uint64_t count = EnvNumber("TISPLE_FUZZ", kDefaultPrograms);
    std::uint64_t first = EnvNumber("TISPLE_FUZZ_SEED", 1);
    FuzzStats stats;
    for (std::uint64_t seed = first; seed < first + count; seed++) {
        FuzzOne(seed, "", GenerateProgram(seed), stats);
        std::string entries = GenerateEntryProgram(seed);
        if (!entries.empty()) {
            FuzzOne(seed, " (entries)", entries, stats);
        }
        std::string late = GenerateLateProgram(seed);
        if (!late.empty()) {
            FuzzOne(seed, " (late)", late, stats);
        }
        std::string dead = GenerateDeadProgram(seed);
        if (!dead.empty()) {
            FuzzOne(seed, " (dead)", dead, stats);
        }
        std::string motion = GenerateMotionProgram(seed);
        if (!motion.empty()) {
            FuzzOne(seed, " (motion)", motion, stats);
        }
        std::string common = GenerateCommonProgram(seed);
        if (!common.empty()) {
            FuzzOne(seed, " (common)", common, stats);
        }
        std::string collapse = GenerateCollapseProgram(seed);
        if (!collapse.empty()) {
            FuzzOne(seed, " (collapse)", collapse, stats);
        }
        std::string values = GenerateValuesProgram(seed);
        if (!values.empty()) {
            FuzzOne(seed, " (values)", values, stats);
        }
        std::string fnptr = GenerateFnPtrProgram(seed);
        if (!fnptr.empty()) {
            FuzzOne(seed, " (fnptr)", fnptr, stats);
        }
        std::string irq = GenerateIrqProgram(seed);
        if (!irq.empty()) {
            FuzzOne(seed, " (irq)", irq, stats, MakeIrqPlan(seed));
        }
        std::string inlined = GenerateInlineProgram(seed);
        if (!inlined.empty()) {
            FuzzOne(seed, " (inline)", inlined, stats);
        }
        std::string sized = GenerateSizeProgram(seed);
        if (!sized.empty()) {
            FuzzOne(seed, " (size)", sized, stats);
        }
        std::string io = GenerateIoProgram(seed);
        if (!io.empty()) {
            FuzzOne(seed, " (io)", io, stats);
            tisple::OptOptions o0;
            o0.level = 0;
            FuzzOne(seed, " (io -O0)", io, stats, IrqPlan(), o0);
        }
    }
    std::printf("fuzz: %llu programs, %llu ran (%llu cycles), %llu skipped at the step limit, "
                "%llu invalid, %llu unsupported, %llu mismatches\n",
                static_cast<unsigned long long>(stats.programs),
                static_cast<unsigned long long>(stats.ran),
                static_cast<unsigned long long>(stats.cycles),
                static_cast<unsigned long long>(stats.skipped),
                static_cast<unsigned long long>(stats.invalid),
                static_cast<unsigned long long>(stats.unsupported),
                static_cast<unsigned long long>(stats.mismatches));
    CHECK(results, stats.invalid == 0);
    CHECK(results, stats.unsupported == 0);
    CHECK(results, stats.mismatches == 0);
    // most programs must actually run, or the fuzzer tests little
    CHECK(results, stats.ran * 4 >= stats.programs * 3);
}

} // namespace tisple_tests
