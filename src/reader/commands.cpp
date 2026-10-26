#include "reader/commands.hpp"

#include "reader/editor_syntax.hpp"
#include "reader/format.hpp"
#include "reader/show.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace tisple {

namespace {

bool WriteFile(const std::string& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream << text;
    return static_cast<bool>(stream);
}

bool Load(const std::string& path, SourceFile& file) {
    std::string error;
    if (!LoadSourceFile(path, file, error)) {
        std::fprintf(stderr, "tisple: %s\n", error.c_str());
        return false;
    }
    return true;
}

} // namespace

int RunFmtCommand(int argc, char** argv) {
    bool write = false;
    bool check = false;
    std::vector<std::string> paths;
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "-w") == 0) {
            write = true;
        } else if (std::strcmp(argv[i], "--check") == 0) {
            check = true;
        } else {
            paths.push_back(argv[i]);
        }
    }
    if (paths.empty() || (write && check)) {
        std::fprintf(stderr, "usage: tisple fmt [-w | --check] file.tsp...\n");
        return 1;
    }

    int status = 0;
    for (const std::string& path : paths) {
        SourceFile file;
        if (!Load(path, file)) {
            status = 1;
            continue;
        }
        std::string formatted;
        ReadError error;
        if (!FormatSource(file, formatted, error)) {
            std::fputs(FormatReadError(file, error).c_str(), stderr);
            status = 1;
            continue;
        }
        bool same = formatted == file.text;
        if (check) {
            if (!same) {
                std::fprintf(stderr, "%s: not formatted\n", path.c_str());
                status = 1;
            }
        } else if (write) {
            if (!same && !WriteFile(path, formatted)) {
                std::fprintf(stderr, "tisple: cannot write %s\n", path.c_str());
                status = 1;
            }
        } else {
            std::fputs(formatted.c_str(), stdout);
        }
    }
    return status;
}

int RunShowCommand(int argc, char** argv) {
    bool color = isatty(fileno(stdout)) != 0 && std::getenv("NO_COLOR") == nullptr;
    std::vector<std::string> paths;
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-color") == 0) {
            color = false;
        } else if (std::strcmp(argv[i], "--color") == 0) {
            color = true;
        } else {
            paths.push_back(argv[i]);
        }
    }
    if (paths.empty()) {
        std::fprintf(stderr, "usage: tisple show [--no-color] file.tsp...\n");
        return 1;
    }

    int status = 0;
    for (std::size_t i = 0; i < paths.size(); i++) {
        SourceFile file;
        if (!Load(paths[i], file)) {
            status = 1;
            continue;
        }
        if (paths.size() > 1) {
            std::printf("%s%s\n", i > 0 ? "\n" : "", paths[i].c_str());
        }
        std::string out;
        if (!RenderShow(file, color, out)) {
            status = 1;
        }
        std::fputs(out.c_str(), stdout);
    }
    return status;
}

int RunGenEditorsCommand(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: tisple gen-editors <editors dir>\n");
        return 1;
    }
    std::string dir = argv[2];
    bool ok = WriteFile(dir + "/" + kVsCodeGrammarPath, VsCodeGrammar()) &&
              WriteFile(dir + "/" + kNvimSyntaxPath, NvimSyntax());
    if (!ok) {
        std::fprintf(stderr, "tisple: cannot write grammars under %s\n", dir.c_str());
        return 1;
    }
    return 0;
}

} // namespace tisple
