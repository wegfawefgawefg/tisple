#include "ide/index.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>

namespace tisple {

namespace {

// the program a file belongs to: the nearest main.tsp in its folder or above. A module's
// imports name paths from the program's folder (tisple build main.tsp), so a module in a
// subfolder only loads as part of its program. Empty when there is none.
std::string ProgramEntry(const std::filesystem::path& file) {
    std::error_code ec;
    for (std::filesystem::path dir = file.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        std::filesystem::path main = dir / "main.tsp";
        if (main != file && std::filesystem::is_regular_file(main, ec)) {
            return main.string();
        }
        if (dir == dir.root_path()) {
            break;
        }
    }
    return "";
}

bool HasFile(const EditorIndex& index, const std::string& path) {
    for (const SourceFile& file : index.sources.files) {
        if (std::filesystem::path(file.path).lexically_normal() == path) {
            return true;
        }
    }
    return false;
}

} // namespace

void IndexFileInProgram(const std::string& path, const LoadOptions& options, EditorIndex& out) {
    std::string file = std::filesystem::absolute(path).lexically_normal().string();
    std::string entry = ProgramEntry(file);
    if (!entry.empty()) {
        IndexProgram(entry, options, out);
    }
    // no program, or one that doesn't import this file: the file is its own program
    if (entry.empty() || !HasFile(out, file)) {
        IndexProgram(file, options, out);
    }
}

int RunIndexCommand(int argc, char** argv) {
    std::string path;
    bool from_stdin = false;
    bool bad = false;
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--stdin") == 0) {
            from_stdin = true;
        } else if (path.empty() && argv[i][0] != '-') {
            path = argv[i];
        } else {
            bad = true;
        }
    }
    if (path.empty() || bad) {
        std::fprintf(stderr, "usage: tisple index file.tsp [--stdin]\n"
                             "  --stdin: file.tsp's text comes from stdin (an unsaved buffer);\n"
                             "           its imports still come from disk\n"
                             "  a file is indexed as part of the program of the nearest main.tsp\n"
                             "  in its folder or above that imports it, else on its own\n");
        return 1;
    }
    std::string file = std::filesystem::absolute(path).lexically_normal().string();
    LoadOptions options;
    if (from_stdin) {
        std::string text((std::istreambuf_iterator<char>(std::cin)),
                         std::istreambuf_iterator<char>());
        options.files.push_back(VirtualFile{file, text});
    }
    EditorIndex index;
    IndexFileInProgram(file, options, index);
    std::fputs(IndexJson(index).c_str(), stdout);
    return 0;
}

} // namespace tisple
