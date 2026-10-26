// generative differential fuzzing: random programs on the 6502 against the interpreter.
// TISPLE_FUZZ=N runs N programs (default 300), TISPLE_FUZZ_SEED picks the first seed.

#include "fuzz_gen.hpp"
#include "test_backend.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

void Report(const char* what, std::uint64_t seed, const std::string& text, const std::string& why,
            std::uint64_t count) {
    if (count > 3) {
        return;
    }
    std::fprintf(stderr, "fuzz %s, seed %llu: %s\n%s\n", what,
                 static_cast<unsigned long long>(seed), why.c_str(), text.c_str());
}

void FuzzOne(std::uint64_t seed, FuzzStats& stats) {
    std::string text = GenerateProgram(seed);
    stats.programs++;
    Compiled c = CompileText(text);
    if (!c.built.ok) {
        stats.invalid++;
        Report("program does not check", seed, text, c.errors, stats.invalid);
        return;
    }
    if (!c.ok) {
        stats.unsupported++;
        Report("program does not compile", seed, text, c.errors, stats.unsupported);
        return;
    }
    Diff d;
    d.in.max_steps = 300000;
    d.max_cycles = 40000000;
    if (!StartDiff(d, c)) {
        stats.mismatches++;
        Report("startup", seed, text, d.message, stats.mismatches);
        return;
    }
    if (DiffCall(d, "t.main", {})) {
        stats.ran++;
        stats.cycles += d.last_cycles;
        return;
    }
    if (d.message.rfind("interpreter stopped", 0) == 0) {
        stats.skipped++;
        return;
    }
    stats.mismatches++;
    Report("mismatch", seed, text, d.message, stats.mismatches);
}

} // namespace

void RunBackendFuzzTests(TestResults& results) {
    std::uint64_t count = EnvNumber("TISPLE_FUZZ", kDefaultPrograms);
    std::uint64_t first = EnvNumber("TISPLE_FUZZ_SEED", 1);
    FuzzStats stats;
    for (std::uint64_t seed = first; seed < first + count; seed++) {
        FuzzOne(seed, stats);
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
