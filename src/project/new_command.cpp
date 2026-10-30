#include "project/new_command.hpp"

#include "project/project_file.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

namespace tisple {

namespace {

namespace fs = std::filesystem;

struct StarterFile {
    const char* name;
    std::string text;
};

std::string ProjectText(const std::string& name) {
    return "; " + name + ": tisple build makes the disk, tisple run plays it\n"
           "(program main.tsp)\n"
           "(disk build/" + name + ".dsk)\n";
}

std::string MainText(const std::string& name) {
    return "; " + name + "\n"
           "\n"
           "(import apple2 a2)\n"
           "(import keys)\n"
           "(import text40)\n"
           "\n"
           "(fn main () void\n"
           "    (text40.on)\n"
           "    (text40.clear)\n"
           "    (text40.put_str \"HELLO FROM TISPLE. ESC QUITS.\")\n"
           "    (while (!= (keys.wait) a2.KEY_ESC)))\n";
}

} // namespace

bool WriteStarterProject(const fs::path& dir, const std::string& name, std::string& error) {
    const StarterFile files[] = {
        {kProjectFileName, ProjectText(name)},
        {"main.tsp", MainText(name)},
        {".gitignore", "build/\n"},
    };
    std::error_code ec;
    for (const StarterFile& file : files) {
        if (fs::exists(dir / file.name, ec)) {
            error = (dir / file.name).string() + " already exists; nothing written";
            return false;
        }
    }
    fs::create_directories(dir, ec);
    if (ec) {
        error = "cannot make " + dir.string() + ": " + ec.message();
        return false;
    }
    for (const StarterFile& file : files) {
        std::ofstream out(dir / file.name);
        out << file.text;
        if (!out) {
            error = "cannot write " + (dir / file.name).string();
            return false;
        }
    }
    return true;
}

int RunNewCommand(int argc, char** argv) {
    bool is_new = std::strcmp(argv[1], "new") == 0;
    if ((is_new && argc != 3) || (!is_new && argc != 2)) {
        std::fprintf(stderr, "usage: tisple new NAME   a folder NAME with a starter project\n"
                             "       tisple init       a starter project in this folder\n");
        return 1;
    }
    fs::path dir = is_new ? fs::path(argv[2]) : fs::current_path();
    std::string name = fs::absolute(dir).lexically_normal().filename().string();
    if (name.empty()) {
        name = fs::absolute(dir).lexically_normal().parent_path().filename().string();
    }
    std::string error;
    if (!WriteStarterProject(dir, name, error)) {
        std::fprintf(stderr, "tisple: %s\n", error.c_str());
        return 1;
    }
    std::printf("made %s: tisple.project, main.tsp, .gitignore\n", dir.string().c_str());
    if (is_new) {
        std::printf("  cd %s && tisple run\n", dir.string().c_str());
    } else {
        std::printf("  tisple run\n");
    }
    return 0;
}

} // namespace tisple
