// the fuzzer's helpers and its single-program runs: TISPLE_FUZZ_FILE and the fuzz farm's
// programs (test_backend_fuzz_internal.hpp)

#include "source_file.hpp"
#include "test_backend_fuzz_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace tisple_tests {

std::uint64_t EnvNumber(const char* name, std::uint64_t fallback) {
    const char* text = std::getenv(name);
    if (!text || !*text) {
        return fallback;
    }
    return std::strtoull(text, nullptr, 0);
}

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
    // TISPLE_FUZZ_BANK1=n: functions in the card's bank 1 as "(bank1 n)" puts them
    tisple::BuildOptions options;
    options.opt = opt;
    options.bank1_test = static_cast<int>(EnvNumber("TISPLE_FUZZ_BANK1", 0));
    // TISPLE_FUZZ_OVERLAY=n: overlay groups as "(overlay n)" makes them
    options.overlay_test = static_cast<int>(EnvNumber("TISPLE_FUZZ_OVERLAY", 0));
    // TISPLE_FUZZ_TIGHT=1: in 2.5 KB of main memory, as "(bank1 n tight)"
    if (EnvNumber("TISPLE_FUZZ_TIGHT", 0) != 0) {
        options.limit = options.origin + 0x0A00;
        options.low_start = options.origin;
    }
    Compiled c = CompileMainWith(file.text, options);
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

} // namespace tisple_tests
