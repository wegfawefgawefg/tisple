#pragma once

// the Apple II libraries of round 1 (docs/roadmap.md, section 5): lib/joystick, lib/machine,
// lib/save and the video modes (lib/dlores, the mixed modes, lib/mousetext), on the built-in
// machine

#include "check.hpp"

namespace tisple_tests {

void RunJoystickLibTests(TestResults& results);

} // namespace tisple_tests
