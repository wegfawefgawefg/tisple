#include "backend/build_command.hpp"

#include "backend/backend.hpp"
#include "backend/sim_run.hpp"
#include "check/check.hpp"
#include "machine/machine_command.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace tisple {

namespace {

struct BuildArgs {
    const char* path = nullptr;
    std::string out;
    BuildOptions options;
    bool listing = false;
    std::uint64_t max_cycles = 100000000;
    bool sim = false;                // run: the bare simulator, not the machine
    bool headless = false;           // run: no window even with SDL
    std::vector<char*> machine_args; // run: flags passed through to the machine
};

// machine flags that mean "no window": the run has an end and leaves files behind
bool IsHeadlessFlag(const char* arg) {
    const char* flags[] = {"--cycles", "--frames", "--screenshot", "--wav", "--save-snapshot"};
    for (const char* flag : flags) {
        if (std::strcmp(arg, flag) == 0) {
            return true;
        }
    }
    return false;
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

bool ParseArgs(int argc, char** argv, bool run, BuildArgs& args) {
    for (int i = 2; i < argc; i++) {
        const char* arg = argv[i];
        bool has_value = i + 1 < argc;
        std::string flag_error;
        if (ParseCodeFlag(argc, argv, i, args.options, flag_error)) {
            if (!flag_error.empty()) {
                std::fprintf(stderr, "tisple: %s\n", flag_error.c_str());
                return false;
            }
            continue;
        }
        std::uint64_t number = 0;
        if (!run && std::strcmp(arg, "--asm") == 0) {
            args.listing = true;
        } else if (!run && std::strcmp(arg, "-o") == 0 && has_value) {
            args.out = argv[++i];
        } else if (std::strcmp(arg, "--origin") == 0 && has_value) {
            if (!ParseNumber(argv[++i], number) || number < 0x0200 || number > 0xBFFF) {
                return false;
            }
            args.options.origin = static_cast<std::uint16_t>(number);
        } else if (run && std::strcmp(arg, "--max-cycles") == 0 && has_value) {
            if (!ParseNumber(argv[++i], number)) {
                return false;
            }
            args.max_cycles = number;
        } else if (run && std::strcmp(arg, "--sim") == 0) {
            args.sim = true;
        } else if (run && std::strcmp(arg, "--headless") == 0) {
            args.headless = true;
        } else if (!args.path && arg[0] != '-') {
            args.path = arg;
        } else if (run && args.path) {
            // everything else after the file belongs to the machine, values included
            args.headless = args.headless || IsHeadlessFlag(arg);
            args.machine_args.push_back(argv[i]);
        } else {
            return false;
        }
    }
    return args.path != nullptr;
}

bool Compile(const BuildArgs& args, Program& program, Compiled& compiled) {
    std::vector<Diagnostic> errors;
    if (!BuildProgram(args.path, LoadOptions(), program, errors) ||
        !CompileProgram(program, args.options, compiled, errors)) {
        std::fputs(FormatDiagnostics(program.sources, errors).c_str(), stderr);
        return false;
    }
    if (args.options.opt.bisect >= 0) {
        std::fputs(DescribeOptReport(compiled.opt).c_str(), stderr);
    }
    std::fputs(FormatDiagnostics(program.sources, compiled.warnings).c_str(), stderr);
    return true;
}

std::string DefaultOut(const char* path) {
    std::string out = path;
    std::size_t dot = out.rfind('.');
    std::size_t slash = out.rfind('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        out = out.substr(0, dot);
    }
    return out + ".bin";
}

void PrintSizes(const Compiled& c) {
    std::fputs(DescribeSegments(c).c_str(), stdout);
}

} // namespace

bool ParseCodeFlag(int argc, char** argv, int& i, BuildOptions& options, std::string& error) {
    if (ParseOptFlag(argc, argv, i, options.opt, error)) {
        return true;
    }
    if (std::strcmp(argv[i], "--no-inline") != 0) {
        return false;
    }
    if (i + 1 >= argc) {
        error = "--no-inline takes a function, module.fn";
        return true;
    }
    options.no_inline.push_back(argv[++i]);
    return true;
}

int RunBuildCommand(int argc, char** argv) {
    BuildArgs args;
    if (!ParseArgs(argc, argv, false, args)) {
        std::fprintf(stderr,
                     "usage: tisple build main.tsp [-o out.bin] [--origin A] [--asm]\n"
                     "  [-O0|-O1] [--opt-bisect N]\n"
                     "  [--no-pass dead|loops|consts|licm|cse|eval]\n"
                     "  [--no-inline module.fn] (a function its calls jump to)\n"
                     "addresses take $hex, 0xhex or decimal\n"
                     "a tisple.project at the project root lets tisple build run with no file\n");
        return 1;
    }
    Program program;
    Compiled compiled;
    if (!Compile(args, program, compiled)) {
        return 1;
    }
    if (args.listing) {
        std::fputs(ListProgram(program, compiled).c_str(), stdout);
    }
    if (!compiled.segments.empty()) {
        // one BRUN file loads one stretch of main memory
        std::fputs(DescribeSegments(compiled).c_str(), stderr);
        std::fprintf(stderr,
                     "%s: the program does not fit one file from $%04X; build a disk that loads "
                     "every part with --disk game.dsk, or run it with tisple run\n",
                     args.path, compiled.origin);
        return 1;
    }
    std::string out = args.out.empty() ? DefaultOut(args.path) : args.out;
    std::FILE* file = std::fopen(out.c_str(), "wb");
    if (!file || std::fwrite(compiled.image.data(), 1, compiled.image.size(), file) !=
                     compiled.image.size()) {
        std::fprintf(stderr, "tisple: cannot write %s\n", out.c_str());
        if (file) {
            std::fclose(file);
        }
        return 1;
    }
    std::fclose(file);
    std::printf("%s: ", out.c_str());
    PrintSizes(compiled);
    std::printf("BRUN %s (loads at $%04X)\n", out.c_str(), compiled.origin);
    return 0;
}

int RunRunCommand(int argc, char** argv, MachineWindowFn window) {
    BuildArgs args;
    if (!ParseArgs(argc, argv, true, args)) {
        std::fprintf(stderr,
                     "usage: tisple run main.tsp [--origin A] [machine flags...]\n"
                     "       tisple run main.tsp --sim [--max-cycles N]\n"
                     "runs in the apple iie window; --frames, --cycles, --screenshot, --wav or\n"
                     "--headless run without one (see tisple machine for every flag)\n"
                     "a tisple.project at the project root lets tisple run run with no file\n");
        return 1;
    }
    Program program;
    Compiled compiled;
    if (!Compile(args, program, compiled)) {
        return 1;
    }
    if (compiled.main < 0) {
        std::fprintf(stderr, "%s: no fn main to run; add (fn main () void ...)\n", args.path);
        return 1;
    }
    if (!args.sim) {
        PrintSizes(compiled);
        static char window_flag[] = "--window";
        if (!args.headless && window != nullptr) {
            args.machine_args.push_back(window_flag);
        }
        return RunMachineOnBinary(compiled.image, compiled.origin, compiled.segments,
                                  static_cast<int>(args.machine_args.size()),
                                  args.machine_args.data(), window);
    }
    SimRunner run;
    SimMain result = RunCompiledMain(run, compiled, args.max_cycles);
    const Function& main = program.functions[static_cast<std::size_t>(compiled.main)];
    if (result.finished) {
        std::uint32_t size = TypeOf(program, main.ret).size;
        if (main.ret != kVoidType && size == 1) {
            std::printf("main returned %u", result.a);
        } else if (main.ret != kVoidType) {
            std::printf("main returned %u", result.a | (result.x << 8));
        } else {
            std::printf("main finished");
        }
        std::printf(" after %llu cycles (%llu with startup); zero page %s\n",
                    static_cast<unsigned long long>(result.main_cycles),
                    static_cast<unsigned long long>(result.total_cycles),
                    result.zp_restored ? "restored" : "NOT restored");
    } else {
        std::printf("stopped: %s after %llu cycles\n", StopReasonName(result.reason),
                    static_cast<unsigned long long>(result.total_cycles));
    }
    PrintSizes(compiled);
    return result.finished && result.zp_restored ? 0 : 2;
}

} // namespace tisple
