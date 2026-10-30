#include "check/commands.hpp"

#include "backend/backend.hpp"
#include "backend/build_command.hpp"
#include "check/check.hpp"
#include "check/expand.hpp"
#include "disk/disk_command.hpp"
#include "disk/image.hpp"
#include "reader/format.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tisple {

namespace {

bool ParseOrigin(const char* text, std::uint16_t& out) {
    int base = 0;
    if (text[0] == '$') {
        text++;
        base = 16;
    }
    char* end = nullptr;
    unsigned long long number = std::strtoull(text, &end, base);
    if (*text == '\0' || *end != '\0' || number < 0x0200 || number > 0xBFFF) {
        return false;
    }
    out = static_cast<std::uint16_t>(number);
    return true;
}

// tisple check's arguments: the file, then the layout flags of tisple build (--origin A,
// --disk [image], which lays it out the way a boot disk loads it) and the code flags, which
// change how much code there is to lay out
bool ParseCheckArgs(int argc, char** argv, const char*& path, BuildOptions& options) {
    for (int i = 2; i < argc; i++) {
        const char* arg = argv[i];
        std::string flag_error;
        if (ParseCodeFlag(argc, argv, i, options, flag_error)) {
            if (!flag_error.empty()) {
                std::fprintf(stderr, "tisple: %s\n", flag_error.c_str());
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--origin") == 0 && i + 1 < argc) {
            if (!ParseOrigin(argv[++i], options.origin)) {
                return false;
            }
        } else if (std::strcmp(arg, "--disk") == 0) {
            UseBootDiskLayout(options);
            // the image a build would write; check writes nothing
            if (i + 1 < argc && DiskFormatForPath(argv[i + 1]) != DiskFormat::kNone) {
                i++;
            }
        } else if (path == nullptr && arg[0] != '-') {
            path = arg;
        } else {
            return false;
        }
    }
    return path != nullptr;
}

} // namespace

int RunCheckCommand(int argc, char** argv) {
    const char* path = nullptr;
    BuildOptions options;
    if (!ParseCheckArgs(argc, argv, path, options)) {
        std::fprintf(stderr, "usage: tisple check file.tsp [--origin A] [--disk [out.dsk]]\n"
                             "  [-O0|-O1] [--no-pass NAME] [--no-inline module.fn]\n"
                             "lays the program out as tisple build does with those flags\n");
        return 1;
    }
    // laid out the way the backend does it, so layout errors match tisple build
    Program program;
    std::vector<Diagnostic> errors;
    bool ok = BuildProgram(path, LoadOptions(), program, errors);
    if (ok) {
        Compiled compiled;
        ok = CompileProgram(program, options, compiled, errors);
        std::fputs(FormatDiagnostics(program.sources, compiled.warnings).c_str(), stderr);
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
