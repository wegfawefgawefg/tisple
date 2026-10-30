#pragma once

// `tisple new NAME` makes the folder NAME with a starter project in it; `tisple init` puts one
// in the current folder. The project: tisple.project (the program and its disk), main.tsp (a
// hello on the text screen, ESC quits), .gitignore (build/). Nothing is overwritten.

#include <filesystem>
#include <string>

namespace tisple {

// writes the starter files into dir, named after name; false with error when one exists
bool WriteStarterProject(const std::filesystem::path& dir, const std::string& name,
                         std::string& error);

int RunNewCommand(int argc, char** argv);

} // namespace tisple
