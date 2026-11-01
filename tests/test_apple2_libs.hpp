#pragma once

// the Apple II libraries of round 1 (docs/roadmap.md, section 5): lib/joystick, lib/machine,
// lib/save and the video modes (lib/dlores, the mixed modes, lib/mousetext), on the built-in
// machine

#include "backend/backend.hpp"
#include "check.hpp"
#include "check/check.hpp"
#include "check/diag.hpp"
#include "machine/apple2.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tisple_tests {

void RunJoystickLibTests(TestResults& results);
void RunMachineLibTests(TestResults& results);
void RunSaveLibTests(TestResults& results);
void RunVideoModeLibTests(TestResults& results);

// t.tsp's text built with the real lib/
struct LibBuild {
    tisple::Program program;
    tisple::Compiled code;
    bool ok = false;
};

bool BuildLibProgram(TestResults& results, const std::string& text,
                     const tisple::BuildOptions& options, LibBuild& b);
// a fresh machine with every segment and the program loaded, ready to start
std::unique_ptr<tisple::Apple2> LoadLibProgram(const LibBuild& b);
// runs until main returns (a check fails otherwise)
bool RunToReturn(TestResults& results, tisple::Apple2& a, uint64_t cycles);
// one of the owner's roms in ~/.local/share/tisple/roms; false when it is not there
bool ReadRomFile(const char* name, std::vector<uint8_t>& out);

} // namespace tisple_tests
