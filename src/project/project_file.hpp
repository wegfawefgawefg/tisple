#pragma once

// tisple.project: a project's build and run flags at its root, so a bare `tisple build`,
// `tisple run` or `tisple check` anywhere inside it works. One (name value) form per flag,
// read with the Tisple reader; paths are from the file's folder:
//   (program game/main.tsp)  required
//   (origin 0x8000)          build, run, check
//   (disk build/catwiz.dsk)  build writes it, run boots it, check lays out for it
//   (mockingboard 4) (scale 2) (taps tools/phone/catwiz.json) (rom Apple2e.rom)   run only

#include <filesystem>
#include <string>
#include <vector>

namespace tisple {

constexpr const char* kProjectFileName = "tisple.project";

// the file's flags as command-line words: paths joined onto the file's folder, numbers in
// decimal; empty when the file does not give one
struct ProjectFile {
    std::string path;
    std::string program;
    std::string origin;
    std::string disk;
    std::string mockingboard;
    std::string scale;
    std::string taps;
    std::string rom;
};

// the nearest tisple.project in dir or a folder above it (dir absolute); "" when none
std::string FindProjectFile(const std::filesystem::path& dir);

// error is "path:line:col: error: ..." with the line and a caret, like any read error
bool LoadProjectFile(const std::string& path, ProjectFile& out, std::string& error);

// the command line `tisple <command>` runs with the project: argv's first two words, the
// program, the project's flags for that command, then argv's other words, which come later
// and so win over the file's
std::vector<std::string> ProjectCommandWords(const ProjectFile& project, int argc, char** argv);

// build, run and check given no .tsp file: words is their command line from the nearest
// tisple.project above the current folder (paths from the current folder), or empty when
// there is none or the command does not take one. False when the project file is bad.
bool ProjectCommandLine(int argc, char** argv, std::vector<std::string>& words, std::string& error);

} // namespace tisple
