#include "backend/build_command.hpp"

#include "backend/backend.hpp"
#include "backend/sim_run.hpp"
#include "check/check.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace tisple {

namespace {

struct BuildArgs {
    const char* path = nullptr;
    std::string out;
    BuildOptions options;
    bool listing = false;
    std::uint64_t max_cycles = 100000000;
};

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
        } else if (!args.path && arg[0] != '-') {
            args.path = arg;
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
    std::printf("%u bytes at $%04X (code %u, data %u); zero page %u bytes (frames %u), "
                "frames in memory %u bytes\n",
                static_cast<unsigned>(c.image.size()), c.origin, c.code_bytes, c.data_bytes,
                c.zp_bytes, c.zp_frames, c.mem_frames);
}

} // namespace

int RunBuildCommand(int argc, char** argv) {
    BuildArgs args;
    if (!ParseArgs(argc, argv, false, args)) {
        std::fprintf(stderr, "usage: tisple build main.tsp [-o out.bin] [--origin A] [--asm]\n"
                             "addresses take $hex, 0xhex or decimal\n");
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

int RunRunCommand(int argc, char** argv) {
    BuildArgs args;
    if (!ParseArgs(argc, argv, true, args)) {
        std::fprintf(stderr, "usage: tisple run main.tsp [--max-cycles N] [--origin A]\n");
        return 1;
    }
    Program program;
    Compiled compiled;
    if (!Compile(args, program, compiled)) {
        return 1;
    }
    if (compiled.main < 0) {
        std::fprintf(stderr, "%s: no fn main to run; add (fn main ...)\n", args.path);
        return 1;
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
