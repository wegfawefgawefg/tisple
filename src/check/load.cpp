#include "check/load.hpp"

#include <filesystem>
#include <unordered_map>

#ifndef TISPLE_LIB_DIR
#define TISPLE_LIB_DIR "lib"
#endif

namespace tisple {

namespace {

enum class LoadState : std::uint8_t {
    Loading,
    Done,
};

struct Loader {
    const LoadOptions* options = nullptr;
    LoadedProgram* out = nullptr;
    std::vector<Diagnostic>* errors = nullptr;
    std::string root_dir;
    std::string lib_dir;
    std::unordered_map<std::string, std::int32_t> by_path;
    std::vector<LoadState> states;
    std::vector<std::int32_t> stack; // modules being loaded, outermost first
};

std::string NormalPath(const std::string& path) {
    return std::filesystem::path(path).lexically_normal().string();
}

const VirtualFile* FindVirtual(const Loader& ld, const std::string& path) {
    for (const VirtualFile& file : ld.options->files) {
        if (NormalPath(file.path) == path) {
            return &file;
        }
    }
    return nullptr;
}

bool FileExists(const Loader& ld, const std::string& path) {
    if (FindVirtual(ld, path)) {
        return true;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

bool ReadSource(Loader& ld, const std::string& path, SourceFile& file, const Loc& from) {
    if (const VirtualFile* virtual_file = FindVirtual(ld, path)) {
        file = SourceFileFromText(path, virtual_file->text);
        return true;
    }
    std::string error;
    if (!LoadSourceFile(path, file, error)) {
        AddError(*ld.errors, from, error);
        return false;
    }
    return true;
}

void AddReadError(Loader& ld, std::uint32_t file_index, const SourceFile& file,
                  const ReadError& error) {
    Loc loc;
    loc.file = file_index;
    loc.span = Span{error.offset, error.offset};
    std::string message = error.message;
    if (error.has_note) {
        SourcePos pos = PosFromOffset(file, error.note_offset);
        message += "; " + error.note + " (line " + std::to_string(pos.line) + ":" +
                   std::to_string(pos.column) + ")";
    }
    AddError(*ld.errors, loc, message);
}

bool IsImport(const Node& form) {
    return form.kind == NodeKind::List && form.mark == QuoteMark::None && !form.items.empty() &&
           form.items[0].kind == NodeKind::Symbol && form.items[0].text == "import";
}

bool IsModulePath(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.back() == '/') {
        return false;
    }
    for (char c : path) {
        if (c == '.' || c == '#') {
            return false;
        }
    }
    return path.find("//") == std::string::npos;
}

std::string LastSegment(const std::string& path) {
    std::size_t cut = path.rfind('/');
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

// gfx/hgr -> first the entry folder, then lib/
std::string FindModuleFile(const Loader& ld, const std::string& path) {
    std::string local = NormalPath((std::filesystem::path(ld.root_dir) / (path + ".tsp")).string());
    if (FileExists(ld, local)) {
        return local;
    }
    std::string lib = NormalPath((std::filesystem::path(ld.lib_dir) / (path + ".tsp")).string());
    if (FileExists(ld, lib)) {
        return lib;
    }
    return "";
}

std::string CycleText(const Loader& ld, std::int32_t target) {
    std::string text;
    bool started = false;
    for (std::int32_t module : ld.stack) {
        started = started || module == target;
        if (started) {
            text += ld.out->modules[static_cast<std::size_t>(module)].name + " -> ";
        }
    }
    return text + ld.out->modules[static_cast<std::size_t>(target)].name;
}

std::int32_t LoadModule(Loader& ld, const std::string& path, const std::string& name,
                        const Loc& from);

bool ReadImport(Loader& ld, std::uint32_t file, const Node& form, Import& import) {
    import.loc = LocOf(file, form);
    std::size_t count = form.items.size();
    if (count < 2 || count > 3 || form.items[1].kind != NodeKind::Symbol ||
        (count == 3 && form.items[2].kind != NodeKind::Symbol)) {
        AddError(*ld.errors, import.loc,
                 "an import is a module path and an optional prefix; write (import gfx/hgr) or "
                 "(import gfx/hgr g)");
        return false;
    }
    import.path = form.items[1].text;
    if (!IsModulePath(import.path)) {
        AddError(*ld.errors, import.loc,
                 import.path + " is not a module path; write folders and a file name without "
                               ".tsp, like gfx/hgr");
        return false;
    }
    import.prefix = count == 3 ? form.items[2].text : LastSegment(import.path);
    return true;
}

void LoadImports(Loader& ld, std::int32_t module_index) {
    std::uint32_t file = ld.out->modules[static_cast<std::size_t>(module_index)].file;
    std::vector<Import> imports;
    const std::vector<Node>& forms =
        ld.out->modules[static_cast<std::size_t>(module_index)].tree.forms;
    for (const Node& form : forms) {
        if (!IsImport(form)) {
            continue;
        }
        Import import;
        if (!ReadImport(ld, file, form, import)) {
            continue;
        }
        bool duplicate = false;
        for (const Import& other : imports) {
            duplicate = duplicate || other.prefix == import.prefix;
        }
        if (duplicate) {
            AddError(*ld.errors, import.loc,
                     "two imports use the prefix " + import.prefix + "; rename one: (import " +
                         import.path + " " + import.prefix + "2)");
            continue;
        }
        std::string found = FindModuleFile(ld, import.path);
        if (found.empty()) {
            AddError(*ld.errors, import.loc,
                     "cannot find module " + import.path + ": no " +
                         NormalPath(ld.root_dir + "/" + import.path + ".tsp") + " or " +
                         NormalPath(ld.lib_dir + "/" + import.path + ".tsp"));
            continue;
        }
        import.module = LoadModule(ld, found, import.path, import.loc);
        imports.push_back(import);
    }
    ld.out->modules[static_cast<std::size_t>(module_index)].imports = imports;
}

std::int32_t LoadModule(Loader& ld, const std::string& path, const std::string& name,
                        const Loc& from) {
    auto known = ld.by_path.find(path);
    if (known != ld.by_path.end()) {
        std::int32_t module = known->second;
        if (ld.states[static_cast<std::size_t>(module)] == LoadState::Loading) {
            AddError(*ld.errors, from,
                     "import cycle: " + CycleText(ld, module) +
                         "; move what both need into a module they both import");
            return -1;
        }
        return module;
    }

    SourceFile source;
    if (!ReadSource(ld, path, source, from)) {
        return -1;
    }
    std::uint32_t file_index = static_cast<std::uint32_t>(ld.out->sources.files.size());
    ld.out->sources.files.push_back(source);
    Module module;
    module.name = name;
    module.file = file_index;
    const SourceFile& read_file = ld.out->sources.files.back();
    if (ld.options->recover) {
        std::vector<ReadError> read_errors;
        ReadTreeRecovering(read_file, module.tree, read_errors);
        for (const ReadError& read_error : read_errors) {
            AddReadError(ld, file_index, read_file, read_error);
        }
    } else {
        ReadError read_error;
        if (!ReadTree(read_file, module.tree, read_error)) {
            AddReadError(ld, file_index, read_file, read_error);
            module.tree = Tree();
        }
    }
    module.tree.comments.clear();

    std::int32_t index = static_cast<std::int32_t>(ld.out->modules.size());
    ld.out->modules.push_back(std::move(module));
    ld.states.push_back(LoadState::Loading);
    ld.by_path[path] = index;
    ld.stack.push_back(index);
    LoadImports(ld, index);
    ld.stack.pop_back();
    ld.states[static_cast<std::size_t>(index)] = LoadState::Done;
    return index;
}

} // namespace

std::string DefaultLibDir() {
    return TISPLE_LIB_DIR;
}

bool LoadProgram(const std::string& entry_path, const LoadOptions& options, LoadedProgram& out,
                 std::vector<Diagnostic>& errors) {
    out = LoadedProgram();
    Loader ld;
    ld.options = &options;
    ld.out = &out;
    ld.errors = &errors;
    std::string entry = NormalPath(entry_path);
    ld.root_dir = std::filesystem::path(entry).parent_path().string();
    if (ld.root_dir.empty()) {
        ld.root_dir = ".";
    }
    ld.lib_dir = options.lib_dir.empty() ? DefaultLibDir() : options.lib_dir;

    std::size_t before = errors.size();
    std::string name = std::filesystem::path(entry).stem().string();
    LoadModule(ld, entry, name, Loc{});
    return errors.size() == before && !out.modules.empty();
}

} // namespace tisple
