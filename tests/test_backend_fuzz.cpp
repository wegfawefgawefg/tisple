// generative differential fuzzing: random programs on the 6502 against the interpreter.
// TISPLE_FUZZ=N runs N programs (default 300), TISPLE_FUZZ_SEED picks the first seed,
// TISPLE_FUZZ_CYCLES=1 prints each program's cycles (to compare two backends seed by seed).
// A seed in four also runs a second program, with loops entered by a jump ("(entries)"), and
// one in four a third, with dead work for the mid-level passes ("(dead)").

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
// the seed's program, " (entries)" or " (dead)" for the others
void Report(const char* what, std::uint64_t seed, const char* variant, const std::string& text,
            const std::string& why, std::uint64_t count) {
    std::fprintf(stderr, "fuzz %s, seed %llu%s: %s\n", what, static_cast<unsigned long long>(seed),
                 variant, why.c_str());
    if (count <= 3) {
        std::fprintf(stderr, "%s\n", text.c_str());
    }
}

void FuzzOne(std::uint64_t seed, const char* variant, const std::string& text, FuzzStats& stats) {
    stats.programs++;
    Compiled c = CompileMainText(text);
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
    if (DiffCall(d, "t.main", {})) {
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
    Compiled c = CompileMainText(file.text);
    Diff d;
    d.in.max_steps = 300000;
    d.max_cycles = 40000000;
    bool ok = StartDiff(d, c) && DiffCall(d, "t.main", {});
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
        Compiled c = CompileMainText(file.text);
        Diff d;
        d.in.max_steps = 300000;
        d.max_cycles = 40000000;
        bool ok = StartDiff(d, c) && DiffCall(d, "t.main", {});
        if (!ok) {
            std::fprintf(stderr, "fuzz farm program %s: %s\n", path.c_str(), d.message.c_str());
        }
        CHECK(results, ok);
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
