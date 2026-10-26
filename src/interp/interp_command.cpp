#include "interp/interp_command.hpp"

#include "backend/backend.hpp"
#include "check/arith.hpp"
#include "check/check.hpp"
#include "interp/interp.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace tisple {

namespace {

std::string ValueText(const Program& program, TypeId type, std::uint16_t value) {
    TypeKind kind = KindOf(program, type);
    if (kind == TypeKind::Bool) {
        return value != 0 ? "true" : "false";
    }
    if (kind == TypeKind::Enum) {
        const EnumDecl& ed = program.enums[static_cast<std::size_t>(TypeOf(program, type).decl)];
        for (const EnumMember& member : ed.members) {
            if (member.value == value) {
                return ed.name + "." + member.name;
            }
        }
    }
    return std::to_string(SignedValue(kind, value));
}

} // namespace

int RunInterpCommand(int argc, char** argv) {
    const char* path = nullptr;
    std::uint64_t max_steps = 10000000;
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            max_steps = std::strtoull(argv[i + 1], nullptr, 10);
            i++;
        } else if (!path) {
            path = argv[i];
        } else {
            path = nullptr;
            break;
        }
    }
    if (!path) {
        std::fprintf(stderr, "usage: tisple interp file.tsp [--max-steps N]\n");
        return 1;
    }

    // the backend's layout, so addresses agree with the compiled program
    Program program;
    Compiled compiled;
    std::vector<Diagnostic> errors;
    if (!BuildProgram(path, LoadOptions(), program, errors) ||
        !CompileProgram(program, BuildOptions(), compiled, errors)) {
        std::fputs(FormatDiagnostics(program.sources, errors).c_str(), stderr);
        return 1;
    }
    if (program.main < 0) {
        std::fprintf(stderr, "%s: no fn main to run; add (fn main ...)\n", path);
        return 1;
    }

    Interp in;
    InitInterp(in, program, compiled.layout);
    in.max_steps = max_steps;
    InterpResult result = CallFunction(in, program.main, {});
    std::string steps = std::to_string(result.steps) + " steps";
    if (result.kind == StopKind::Finished) {
        if (result.has_value) {
            std::printf("main returned %s (%s) after %s\n",
                        ValueText(program, result.type, result.value).c_str(),
                        TypeName(program, result.type).c_str(), steps.c_str());
        } else {
            std::printf("main finished after %s\n", steps.c_str());
        }
        return 0;
    }
    std::printf("stopped: %s at %s\n", result.message.c_str(),
                FormatLoc(program.sources, result.loc).c_str());
    return 2;
}

} // namespace tisple
