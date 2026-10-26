#pragma once

#include "check/diag.hpp"

#include <string>
#include <vector>

namespace tisple {

struct Import {
    std::string path;   // as written: gfx/hgr
    std::string prefix; // hgr, or the second argument
    Loc loc;
    std::int32_t module = -1;
};

struct Module {
    std::string name;       // import path, or the entry file's stem
    std::uint32_t file = 0; // into Sources::files
    Tree tree;              // top-level forms; macros are expanded in place
    std::vector<Import> imports;
    std::vector<std::string> macro_names; // defined here, removed from tree by expansion
};

struct LoadedProgram {
    Sources sources;
    std::vector<Module> modules; // entry first
};

// a file given as text, for tests: looked up by path before the disk
struct VirtualFile {
    std::string path;
    std::string text;
};

struct LoadOptions {
    std::string lib_dir; // empty: the repo's lib/ compiled in as TISPLE_LIB_DIR
    std::vector<VirtualFile> files;
};

// reads the entry file and every module it imports, relative to the entry file's folder
// and then lib/. Fails on read errors, missing modules and import cycles.
bool LoadProgram(const std::string& entry_path, const LoadOptions& options, LoadedProgram& out,
                 std::vector<Diagnostic>& errors);

std::string DefaultLibDir();

} // namespace tisple
