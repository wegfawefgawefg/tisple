#include "test_project.hpp"

#include "ide/index.hpp"
#include "project/new_command.hpp"
#include "project/project_file.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace tisple_tests {

namespace {

namespace fs = std::filesystem;

void WriteText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// a project in a temp folder: the program in game/, a module it imports in game/battle/
fs::path MakeProject() {
    fs::path root = fs::temp_directory_path() / ("tisple_project_" + std::to_string(getpid()));
    fs::remove_all(root);
    WriteText(root / "tisple.project", "; a test project\n"
                                       "(program game/main.tsp)\n"
                                       "(origin 0x8000)\n"
                                       "(disk build/test.dsk)\n"
                                       "(mockingboard 4)\n"
                                       "(scale 2)\n"
                                       "(taps \"tools/taps.json\")\n");
    WriteText(root / "game/main.tsp", "(import battle/fight)\n"
                                      "(fn main () void (fight.hit))\n");
    WriteText(root / "game/battle/fight.tsp", "(fn hit () void)\n");
    return root;
}

void LoadTests(TestResults& results, const fs::path& root) {
    std::string path = (root / "tisple.project").string();
    std::string folder = root.string() + "/";
    tisple::ProjectFile project;
    std::string error;
    CHECK(results, tisple::LoadProjectFile(path, project, error));
    CHECK(results, project.program == folder + "game/main.tsp");
    CHECK(results, project.origin == "32768");
    CHECK(results, project.disk == folder + "build/test.dsk");
    CHECK(results, project.mockingboard == "4");
    CHECK(results, project.scale == "2");
    CHECK(results, project.taps == folder + "tools/taps.json");
    CHECK(results, project.rom.empty());

    // a path from the current folder keeps its folder; one with none has no prefix
    std::string here = (root / "here.project").string();
    WriteText(here, "(program main.tsp)\n(rom /roms/Apple2e.rom)\n");
    CHECK(results, tisple::LoadProjectFile(here, project, error));
    CHECK(results, project.program == folder + "main.tsp");
    CHECK(results, project.rom == "/roms/Apple2e.rom");

    // every problem is an error at its place in the file
    struct Bad {
        const char* text;
        const char* error;
    };
    const Bad bad[] = {
        {"(program main.tsp)\n(orign 0x8000)\n", "bad.project:2:2: error: unknown form 'orign'"},
        {"(program main.tsp)\n(scale 9)\n", "bad.project:2:8: error: scale takes a number"},
        {"(program main.tsp)\n(origin main)\n", "bad.project:2:9: error: origin takes a number"},
        {"(program main.tsp)\n(disk 4)\n", "bad.project:2:7: error: disk takes a path"},
        {"(program a.tsp)\n(program b.tsp)\n", "bad.project:2:2: error: (program ...) is given"},
        {"(program a.tsp b.tsp)\n", "bad.project:1:1: error: (program ...) takes one value"},
        {"program\n", "bad.project:1:1: error: a tisple.project holds (name value) forms"},
        {"(origin 0x8000)\n", "bad.project: error: no (program file.tsp)"},
        {"(program main.tsp\n", "bad.project:1:1: error:"},
    };
    std::string bad_path = (root / "bad.project").string();
    for (const Bad& entry : bad) {
        WriteText(bad_path, entry.text);
        CHECK(results, !tisple::LoadProjectFile(bad_path, project, error));
        CHECK(results, Contains(error, entry.error));
    }
    CHECK(results, !tisple::LoadProjectFile((root / "none.project").string(), project, error));
}

void FindTests(TestResults& results, const fs::path& root) {
    std::string expected = (root / "tisple.project").string();
    CHECK(results, tisple::FindProjectFile(root) == expected);
    CHECK(results, tisple::FindProjectFile(root / "game/battle") == expected);
    CHECK(results, tisple::FindProjectFile(root.parent_path()).empty());
}

void WordsTests(TestResults& results) {
    tisple::ProjectFile project;
    project.program = "../game/main.tsp";
    project.origin = "32768";
    project.disk = "../build/a.dsk";
    project.scale = "2";
    project.taps = "../taps.json";
    char tisple_word[] = "tisple";
    char build_word[] = "build";
    char run_word[] = "run";
    char asm_word[] = "--asm";
    char scale_word[] = "--scale";
    char three_word[] = "3";

    // build takes the program, origin and disk; the command line comes after
    char* build[] = {tisple_word, build_word, asm_word};
    std::vector<std::string> words = tisple::ProjectCommandWords(project, 3, build);
    std::vector<std::string> expect = {"tisple", "build",  "../game/main.tsp", "--origin",
                                       "32768",  "--disk", "../build/a.dsk",   "--asm"};
    CHECK(results, words == expect);

    // run takes the machine flags too; the command line's --scale comes later and wins
    char* run[] = {tisple_word, run_word, scale_word, three_word};
    words = tisple::ProjectCommandWords(project, 4, run);
    expect = {
        "tisple",  "run", "../game/main.tsp", "--origin",     "32768",   "--disk", "../build/a.dsk",
        "--scale", "2",   "--taps",           "../taps.json", "--scale", "3"};
    CHECK(results, words == expect);

    // with a .tsp file, or for another command, there is no project command line
    char file_word[] = "other.tsp";
    char fmt_word[] = "fmt";
    char* with_file[] = {tisple_word, build_word, file_word};
    std::string error;
    CHECK(results, tisple::ProjectCommandLine(3, with_file, words, error) && words.empty());
    char* fmt[] = {tisple_word, fmt_word};
    CHECK(results, tisple::ProjectCommandLine(2, fmt, words, error) && words.empty());
}

// a module opened in the editor loads as part of the project's program, with no main.tsp
// in the module's folder or above
void IndexTests(TestResults& results, const fs::path& root) {
    fs::rename(root / "game/main.tsp", root / "game/play.tsp");
    WriteText(root / "tisple.project", "(program game/play.tsp)\n");
    tisple::EditorIndex index;
    tisple::IndexFileInProgram((root / "game/battle/fight.tsp").string(), tisple::LoadOptions(),
                               index);
    CHECK(results, index.errors.empty());
    CHECK(results, index.sources.files.size() == 2);
    CHECK(results, !index.sources.files.empty() &&
                       index.sources.files[0].path == (root / "game/play.tsp").string());

    // a file the project's program does not import is its own program
    WriteText(root / "game/loose.tsp", "(fn loose () void)\n");
    tisple::IndexFileInProgram((root / "game/loose.tsp").string(), tisple::LoadOptions(), index);
    CHECK(results, index.errors.empty());
    CHECK(results, index.sources.files.size() == 1);
}

} // namespace

// tisple new: the starter project loads, and a second one into the same folder writes nothing
void NewTests(TestResults& results, const fs::path& root) {
    fs::path dir = root / "starter";
    std::string error;
    CHECK(results, tisple::WriteStarterProject(dir, "starter", error));
    tisple::ProjectFile project;
    CHECK(results, tisple::LoadProjectFile((dir / "tisple.project").string(), project, error));
    CHECK(results, project.program == (dir / "main.tsp").string());
    CHECK(results, project.disk == (dir / "build/starter.dsk").string());
    tisple::EditorIndex index;
    tisple::IndexFileInProgram((dir / "main.tsp").string(), tisple::LoadOptions(), index);
    CHECK(results, index.errors.empty());
    CHECK(results, !tisple::WriteStarterProject(dir, "starter", error));
    CHECK(results, Contains(error, "already exists"));
}

void RunProjectTests(TestResults& results) {
    fs::path root = MakeProject();
    LoadTests(results, root);
    FindTests(results, root);
    WordsTests(results);
    IndexTests(results, root);
    NewTests(results, root);
    fs::remove_all(root);
}

} // namespace tisple_tests
