#pragma once

#include "check.hpp"

namespace tisple_tests {

// tisple race: timing a kernel inside a binary
void RunRaceTests(TestResults& results);
// the random-program race: the generator's race mode and its programs as C
void RunRaceCTests(TestResults& results);

} // namespace tisple_tests
