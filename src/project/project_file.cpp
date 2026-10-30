#include "project/project_file.hpp"

#include "reader/reader.hpp"
#include "source_file.hpp"

#include <cstring>

namespace tisple {

namespace {

enum class ValueKind { kPath, kNumber };

// one form a tisple.project may hold: its name, what it takes, the flag it stands for and
// whether only tisple run takes that flag
struct ProjectForm {
    const char* name;
    ValueKind kind;
    std::int64_t low;
    std::int64_t high;
    std::string ProjectFile::*field;
    const char* flag;
    bool run_only;
};

const ProjectForm kForms[] = {
    {"program", ValueKind::kPath, 0, 0, &ProjectFile::program, nullptr, false},
    {"origin", ValueKind::kNumber, 0x0200, 0xBFFF, &ProjectFile::origin, "--origin", false},
    {"disk", ValueKind::kPath, 0, 0, &ProjectFile::disk, "--disk", false},
    {"mockingboard", ValueKind::kNumber, 1, 7, &ProjectFile::mockingboard, "--mockingboard", true},
    {"scale", ValueKind::kNumber, 1, 8, &ProjectFile::scale, "--scale", true},
    {"taps", ValueKind::kPath, 0, 0, &ProjectFile::taps, "--taps", true},
    {"rom", ValueKind::kPath, 0, 0, &ProjectFile::rom, "--rom", true},
};

const ProjectForm* FindForm(const std::string& name) {
    for (const ProjectForm& form : kForms) {
        if (name == form.name) {
            return &form;
        }
    }
    return nullptr;
}

std::string FormError(const SourceFile& file, const Node& node, const std::string& message) {
    ReadError error;
    error.offset = node.span.start;
    error.message = message;
    return FormatReadError(file, error);
}

// the folder part of path, with its slash: "a/b/" for "a/b/tisple.project", "" for none
std::string FolderOf(const std::string& path) {
    std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

// one (name value) form into out; false with error when it is not one the file may hold
bool ReadForm(const SourceFile& file, const Node& node, const std::string& folder, ProjectFile& out,
              std::string& error) {
    if (node.kind != NodeKind::List || node.items.empty() ||
        node.items[0].kind != NodeKind::Symbol) {
        error = FormError(file, node,
                          "a tisple.project holds (name value) forms, like "
                          "(program main.tsp)");
        return false;
    }
    const Node& name = node.items[0];
    const ProjectForm* form = FindForm(name.text);
    if (form == nullptr) {
        error = FormError(file, name,
                          "unknown form '" + name.text +
                              "'; a tisple.project takes program, origin, disk, "
                              "mockingboard, scale, taps and rom");
        return false;
    }
    if (!(out.*form->field).empty()) {
        error = FormError(file, name, "(" + name.text + " ...) is given twice");
        return false;
    }
    if (node.items.size() != 2) {
        error = FormError(file, node, "(" + name.text + " ...) takes one value");
        return false;
    }
    const Node& value = node.items[1];
    if (form->kind == ValueKind::kNumber) {
        if (value.kind != NodeKind::Integer || value.value < form->low ||
            value.value > form->high) {
            error = FormError(file, value,
                              name.text + " takes a number from " + std::to_string(form->low) +
                                  " to " + std::to_string(form->high));
            return false;
        }
        out.*form->field = std::to_string(value.value);
        return true;
    }
    std::string path;
    if (value.kind == NodeKind::Symbol) {
        path = value.text;
    } else if (value.kind == NodeKind::String) {
        path = value.bytes;
    }
    if (path.empty()) {
        error = FormError(file, value, name.text + " takes a path, like game/main.tsp");
        return false;
    }
    // paths are from the project file's folder
    out.*form->field = path[0] == '/' ? path : folder + path;
    return true;
}

// a word naming a source file: the command then runs exactly as without a project
bool HasSourceArg(int argc, char** argv) {
    for (int i = 2; i < argc; i++) {
        std::size_t length = std::strlen(argv[i]);
        if (argv[i][0] != '-' && length > 4 && std::strcmp(argv[i] + length - 4, ".tsp") == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string FindProjectFile(const std::filesystem::path& dir) {
    std::error_code ec;
    for (std::filesystem::path at = dir; !at.empty(); at = at.parent_path()) {
        std::filesystem::path project = at / kProjectFileName;
        if (std::filesystem::is_regular_file(project, ec)) {
            return project.string();
        }
        if (at == at.root_path()) {
            break;
        }
    }
    return "";
}

bool LoadProjectFile(const std::string& path, ProjectFile& out, std::string& error) {
    out = ProjectFile();
    out.path = path;
    SourceFile file;
    if (!LoadSourceFile(path, file, error)) {
        error = "tisple: " + error + "\n";
        return false;
    }
    Tree tree;
    ReadError read_error;
    if (!ReadTree(file, tree, read_error)) {
        error = FormatReadError(file, read_error);
        return false;
    }
    std::string folder = FolderOf(path);
    for (const Node& node : tree.forms) {
        if (!ReadForm(file, node, folder, out, error)) {
            return false;
        }
    }
    if (out.program.empty()) {
        error = path + ": error: no (program file.tsp), the program to build\n";
        return false;
    }
    return true;
}

std::vector<std::string> ProjectCommandWords(const ProjectFile& project, int argc, char** argv) {
    bool run = std::strcmp(argv[1], "run") == 0;
    std::vector<std::string> words = {argv[0], argv[1], project.program};
    for (const ProjectForm& form : kForms) {
        const std::string& value = project.*form.field;
        if (form.flag == nullptr || value.empty() || (form.run_only && !run)) {
            continue;
        }
        words.push_back(form.flag);
        words.push_back(value);
    }
    for (int i = 2; i < argc; i++) {
        words.push_back(argv[i]);
    }
    return words;
}

bool ProjectCommandLine(int argc, char** argv, std::vector<std::string>& words,
                        std::string& error) {
    words.clear();
    const char* command = argv[1];
    bool takes_project = std::strcmp(command, "build") == 0 || std::strcmp(command, "run") == 0 ||
                         std::strcmp(command, "check") == 0;
    if (!takes_project || HasSourceArg(argc, argv)) {
        return true;
    }
    std::error_code ec;
    std::filesystem::path here = std::filesystem::current_path(ec);
    if (ec) {
        return true;
    }
    std::string found = FindProjectFile(here);
    if (found.empty()) {
        return true;
    }
    // paths from the current folder, so messages name files the way the user would
    std::string path = std::filesystem::path(found).lexically_relative(here).string();
    ProjectFile project;
    if (!LoadProjectFile(path, project, error)) {
        return false;
    }
    words = ProjectCommandWords(project, argc, argv);
    return true;
}

} // namespace tisple
