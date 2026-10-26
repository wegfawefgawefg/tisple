#include "check/check.hpp"
#include "gaps/gaps.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tisple {

namespace {

struct GapsArgs {
    const char* path = nullptr;
    GapsOptions options;
    BuildOptions build;
    std::uint64_t fuzz = 0; // programs; 0 for a file
    std::uint64_t seed = 1;
    int threads = 1;
    std::size_t top = 25;
    bool cycles_given = false;
    bool print_source = false; // --fuzz: print the seed's program instead
};

void PrintGapsUsage() {
    std::fprintf(
        stderr,
        "usage: tisple gaps prog.tsp [--cycles N] [--sim] [--superopt] [--window N] [options]\n"
        "       tisple gaps --fuzz N [--seed S] [--threads T] [--cycles N] [--superopt] ...\n"
        "profiles the compiled program (on the Apple IIe, or --sim the bare simulator; a\n"
        "program without main has every function called in turn) and ranks, by cycles, the\n"
        "instructions some fact would make redundant. --superopt also searches the hot windows\n"
        "for cheaper sequences. --fuzz runs the fuzz generator's programs and adds them up.\n"
        "options: --top N (rows), --min-count N (executions before judging, 16),\n"
        "         --min-percent P (100), --windows N (window starts searched, 30),\n"
        "         --nodes N (search budget per window, 1000000), --max-len N (5),\n"
        "         --stochastic STEPS (MCMC instead of enumeration), --no-facts,\n"
        "         --only NAME (functions whose name contains NAME),\n"
        "         --verbose (each search on stderr), --print (--fuzz: print the program of\n"
        "         --seed, to build and read it as t.tsp),\n"
        "         --origin A\n");
}

bool ParseNumber(const char* text, std::uint64_t& out) {
    int base = 0;
    if (text[0] == '$') {
        text++;
        base = 16;
    }
    char* end = nullptr;
    out = std::strtoull(text, &end, base);
    return *text != '\0' && *end == '\0';
}

// a flag with a number after it; false when the flag is not this one
bool NumberFlag(int argc, char** argv, int& i, const char* flag, std::uint64_t& out, bool& ok) {
    if (std::strcmp(argv[i], flag) != 0) {
        return false;
    }
    ok = i + 1 < argc && ParseNumber(argv[i + 1], out);
    i++;
    return true;
}

bool ParseArgs(int argc, char** argv, GapsArgs& args) {
    GapsOptions& o = args.options;
    for (int i = 2; i < argc; i++) {
        const char* arg = argv[i];
        std::uint64_t n = 0;
        bool ok = true;
        if (std::strcmp(arg, "--superopt") == 0) {
            o.superopt = true;
        } else if (std::strcmp(arg, "--sim") == 0) {
            o.run.bare = true;
        } else if (std::strcmp(arg, "--only") == 0 && i + 1 < argc) {
            o.only = argv[++i];
        } else if (std::strcmp(arg, "--print") == 0) {
            args.print_source = true;
        } else if (std::strcmp(arg, "--verbose") == 0) {
            o.windows.verbose = true;
        } else if (std::strcmp(arg, "--no-facts") == 0) {
            o.windows.facts = false;
        } else if (NumberFlag(argc, argv, i, "--cycles", n, ok)) {
            o.run.max_cycles = n;
            args.cycles_given = true;
        } else if (NumberFlag(argc, argv, i, "--window", n, ok)) {
            ok = ok && n >= 2 && n <= 12;
            o.windows.window = static_cast<int>(n);
        } else if (NumberFlag(argc, argv, i, "--windows", n, ok)) {
            o.windows.top = static_cast<int>(n);
        } else if (NumberFlag(argc, argv, i, "--nodes", n, ok)) {
            o.windows.search.max_nodes = n;
        } else if (NumberFlag(argc, argv, i, "--max-len", n, ok)) {
            ok = ok && n >= 1 && n <= 12;
            o.windows.search.max_len = static_cast<int>(n);
        } else if (NumberFlag(argc, argv, i, "--stochastic", n, ok)) {
            o.windows.search.stochastic_steps = n;
        } else if (NumberFlag(argc, argv, i, "--top", n, ok)) {
            args.top = static_cast<std::size_t>(n);
        } else if (NumberFlag(argc, argv, i, "--min-count", n, ok)) {
            o.find.min_count = n;
            o.windows.min_count = n;
        } else if (NumberFlag(argc, argv, i, "--min-percent", n, ok)) {
            ok = ok && n >= 1 && n <= 100;
            o.find.min_fraction = static_cast<double>(n) / 100.0;
        } else if (NumberFlag(argc, argv, i, "--fuzz", n, ok)) {
            args.fuzz = n;
        } else if (NumberFlag(argc, argv, i, "--seed", n, ok)) {
            args.seed = n;
        } else if (NumberFlag(argc, argv, i, "--threads", n, ok)) {
            ok = ok && n >= 1 && n <= 64;
            args.threads = static_cast<int>(n);
        } else if (NumberFlag(argc, argv, i, "--origin", n, ok)) {
            ok = ok && n >= 0x0200 && n <= 0xBFFF;
            args.build.origin = static_cast<std::uint16_t>(n);
        } else if (arg[0] != '-' && args.path == nullptr) {
            args.path = arg;
        } else {
            return false;
        }
        if (!ok) {
            return false;
        }
    }
    return (args.path != nullptr) != (args.fuzz > 0);
}

int RunFuzz(GapsArgs& args, ProgramTextFn generate) {
    if (generate == nullptr) {
        std::fprintf(stderr, "tisple gaps: this build has no fuzz generator\n");
        return 1;
    }
    if (args.print_source) {
        std::fputs(generate(args.seed).c_str(), stdout);
        return 0;
    }
    if (!args.cycles_given) {
        args.options.run.max_cycles = 2000000; // a fuzz main runs well under this
    }
    auto start = std::chrono::steady_clock::now();
    BulkGaps bulk = FuzzGaps(generate, args.seed, args.fuzz, args.threads, args.options);
    double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::fputs(FormatBulkReport(bulk, args.seed, args.top).c_str(), stdout);
    std::printf("\n%.1f s on %d threads\n", seconds, args.threads);
    return 0;
}

} // namespace

int RunGapsCommand(int argc, char** argv, ProgramTextFn generate) {
    GapsArgs args;
    if (!ParseArgs(argc, argv, args)) {
        PrintGapsUsage();
        return 1;
    }
    if (args.fuzz > 0) {
        return RunFuzz(args, generate);
    }
    Program program;
    Compiled compiled;
    std::vector<Diagnostic> errors;
    if (!BuildProgram(args.path, LoadOptions(), program, errors) ||
        !CompileProgram(program, args.build, compiled, errors)) {
        std::fputs(FormatDiagnostics(program.sources, errors).c_str(), stderr);
        return 1;
    }
    WindowCache cache;
    ProgramGaps gaps;
    gaps.name = args.path;
    std::string error;
    if (!FindProgramGaps(program, compiled, args.options, cache, gaps, error)) {
        std::fprintf(stderr, "tisple gaps: %s\n", error.c_str());
        return 1;
    }
    std::fputs(FormatProgramReport(gaps, args.top).c_str(), stdout);
    return 0;
}

} // namespace tisple
