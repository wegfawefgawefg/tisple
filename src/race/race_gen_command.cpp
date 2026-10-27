#include "backend/backend.hpp"
#include "check/check.hpp"
#include "interp/interp.hpp"
#include "race/race_c.hpp"
#include "race/race_gen.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

// tisple race-gen (race_gen.hpp)

namespace tisple {

namespace {

struct RaceGenArgs {
    std::uint64_t seed = 1;
    std::uint64_t count = 1;
    std::uint64_t max_steps = 3000000;
    std::string out;
    bool print = false; // the seed's Tisple program on stdout
    bool print_c = false;
};

struct RaceProgram {
    std::string status = "ok"; // ok, step_limit, does_not_check, does_not_compile, no_c, ...
    std::string message;
    std::string tisple;
    std::string c;
    std::uint16_t checksum = 0;
    std::uint64_t steps = 0;
    std::map<std::string, std::uint64_t> features; // as written
    std::map<std::string, std::uint64_t> work;     // weighed by how often each part ran
};

void PrintRaceGenUsage() {
    std::fprintf(stderr,
                 "usage: tisple race-gen --seed S [--count N] --out DIR [--max-steps N]\n"
                 "       tisple race-gen --seed S --print | --print-c\n"
                 "the random-program race's programs: for each seed from S, DIR/<seed>/t.tsp,\n"
                 "the same program in C as t.c (with races/include/random.h), and info.json\n"
                 "with the interpreter's checksum (the u16 t.checksum leaves at $BF00), its\n"
                 "steps, the features t.run uses, and the same weighed by how often each ran.\n"
                 "--print writes one seed's program\n");
}

bool ParseNumber(const char* text, std::uint64_t& out) {
    char* end = nullptr;
    out = std::strtoull(text, &end, 0);
    return *text != '\0' && *end == '\0';
}

bool ParseArgs(int argc, char** argv, RaceGenArgs& args) {
    for (int i = 2; i < argc; i++) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--print") == 0) {
            args.print = true;
            continue;
        }
        if (std::strcmp(arg, "--print-c") == 0) {
            args.print_c = true;
            continue;
        }
        if (i + 1 >= argc) {
            return false;
        }
        const char* value = argv[++i];
        bool ok = true;
        if (std::strcmp(arg, "--seed") == 0) {
            ok = ParseNumber(value, args.seed);
        } else if (std::strcmp(arg, "--count") == 0) {
            ok = ParseNumber(value, args.count);
        } else if (std::strcmp(arg, "--max-steps") == 0) {
            ok = ParseNumber(value, args.max_steps);
        } else if (std::strcmp(arg, "--out") == 0) {
            args.out = value;
        } else {
            return false;
        }
        if (!ok) {
            return false;
        }
    }
    return args.print || args.print_c || !args.out.empty();
}

RaceProgram MakeProgram(RaceTextFn generate, std::uint64_t seed, std::uint64_t max_steps) {
    RaceProgram out;
    out.tisple = generate(seed);
    LoadOptions load;
    load.lib_dir = "lib0"; // nothing to import: generated programs stand alone
    load.files.push_back(VirtualFile{"t.tsp", out.tisple});
    Program program;
    Compiled compiled;
    std::vector<Diagnostic> errors;
    if (!BuildProgram("t.tsp", load, program, errors)) {
        out.status = "does_not_check";
        out.message = FormatDiagnostics(program.sources, errors);
        return out;
    }
    out.features = RaceFeatures(program, nullptr);
    std::string error;
    if (!WriteRaceC(program, out.c, error)) {
        out.status = "no_c";
        out.message = error;
        return out;
    }
    BuildOptions options;
    options.origin = 0x4000; // as races/random_build.py builds it: above the hi-res page
    if (!CompileProgram(program, options, compiled, errors)) {
        out.status = "does_not_compile";
        out.message = FormatDiagnostics(program.sources, errors);
        return out;
    }
    std::int32_t chk = FindVar(program, "t.chk");
    if (chk < 0 || program.main < 0) {
        out.status = "no_checksum";
        return out;
    }
    Interp in;
    in.max_steps = max_steps;
    std::vector<std::uint32_t> counts(program.exprs.size(), 0);
    in.expr_counts = &counts;
    InitInterp(in, program, compiled.layout);
    InterpResult result = CallFunction(in, program.main, {});
    out.steps = result.steps;
    if (result.kind == StopKind::StepLimit) {
        out.status = "step_limit";
        return out;
    }
    if (result.kind != StopKind::Finished) {
        out.status = "interp_error";
        out.message = result.message;
        return out;
    }
    out.checksum = ReadScalar(in, VarCursor(compiled.layout, chk), 2);
    out.work = RaceFeatures(program, &counts);
    return out;
}

std::string JsonString(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string JsonCounts(const std::map<std::string, std::uint64_t>& counts) {
    std::string text = "{";
    for (const auto& [name, count] : counts) {
        text += (text.size() == 1 ? "" : ", ") + JsonString(name) + ": " + std::to_string(count);
    }
    return text + "}";
}

std::string InfoJson(std::uint64_t seed, const RaceProgram& p) {
    std::string text =
        "{\"seed\": " + std::to_string(seed) + ", \"status\": " + JsonString(p.status) +
        ", \"checksum\": " + std::to_string(p.checksum) +
        ", \"steps\": " + std::to_string(p.steps) + ", \"features\": " + JsonCounts(p.features) +
        ", \"work\": " + JsonCounts(p.work);
    if (!p.message.empty()) {
        text += ", \"message\": " + JsonString(p.message);
    }
    return text + "}\n";
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    return std::fclose(file) == 0 && ok;
}

bool WriteProgram(const std::string& out, std::uint64_t seed, const RaceProgram& p) {
    std::filesystem::path dir = std::filesystem::path(out) / std::to_string(seed);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string c = "/* seed " + std::to_string(seed) +
                    ": t.tsp in C, by tisple race-gen (src/race/race_c.hpp) */\n" + p.c;
    return WriteText(dir / "t.tsp", p.tisple) && WriteText(dir / "t.c", c) &&
           WriteText(dir / "info.json", InfoJson(seed, p));
}

} // namespace

int RunRaceGenCommand(int argc, char** argv, RaceTextFn generate) {
    RaceGenArgs args;
    if (!ParseArgs(argc, argv, args)) {
        PrintRaceGenUsage();
        return 1;
    }
    if (args.print || args.print_c) {
        RaceProgram p = MakeProgram(generate, args.seed, args.max_steps);
        std::fputs(args.print ? p.tisple.c_str() : p.c.c_str(), stdout);
        if (p.status != "ok") {
            std::fprintf(stderr, "%s %s\n", p.status.c_str(), p.message.c_str());
        }
        return 0;
    }
    for (std::uint64_t seed = args.seed; seed < args.seed + args.count; seed++) {
        RaceProgram p = MakeProgram(generate, seed, args.max_steps);
        if (!WriteProgram(args.out, seed, p)) {
            std::fprintf(stderr, "tisple race-gen: cannot write into %s\n", args.out.c_str());
            return 1;
        }
    }
    return 0;
}

} // namespace tisple
