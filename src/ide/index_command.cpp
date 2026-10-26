#include "ide/index.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <iterator>

namespace tisple {

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
                             "           its imports still come from disk\n");
        return 1;
    }
    LoadOptions options;
    if (from_stdin) {
        std::string text((std::istreambuf_iterator<char>(std::cin)),
                         std::istreambuf_iterator<char>());
        options.files.push_back(VirtualFile{path, text});
    }
    EditorIndex index;
    IndexProgram(path, options, index);
    std::fputs(IndexJson(index).c_str(), stdout);
    return 0;
}

} // namespace tisple
