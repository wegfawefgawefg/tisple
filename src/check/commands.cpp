#include "check/commands.hpp"

#include "backend/backend.hpp"
#include "check/check.hpp"
#include "check/expand.hpp"
#include "reader/format.hpp"

#include <cstdio>

namespace tisple {

int RunCheckCommand(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: tisple check file.tsp\n");
        return 1;
    }
    // laid out the way the backend does it, so layout errors match tisple build
    Program program;
    std::vector<Diagnostic> errors;
    bool ok = BuildProgram(argv[2], LoadOptions(), program, errors);
    if (ok) {
        Compiled compiled;
        ok = CompileProgram(program, BuildOptions(), compiled, errors);
    }
    if (!ok) {
        std::fputs(FormatDiagnostics(program.sources, errors).c_str(), stderr);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}

int RunExpandCommand(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: tisple expand file.tsp\n");
        return 1;
    }
    LoadedProgram loaded;
    std::vector<Diagnostic> errors;
    if (!LoadProgram(argv[2], LoadOptions(), loaded, errors) || !ExpandProgram(loaded, errors)) {
        std::fputs(FormatDiagnostics(loaded.sources, errors).c_str(), stderr);
        return 1;
    }
    // every module, entry first; a module header when there are several
    for (std::size_t i = 0; i < loaded.modules.size(); i++) {
        const Module& module = loaded.modules[i];
        const SourceFile& file = loaded.sources.files[module.file];
        if (loaded.modules.size() > 1) {
            std::printf("%s; module %s (%s)\n", i == 0 ? "" : "\n", module.name.c_str(),
                        file.path.c_str());
        }
        std::fputs(FormatTree(file, module.tree, false).c_str(), stdout);
    }
    return 0;
}

} // namespace tisple
