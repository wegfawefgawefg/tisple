#pragma once

#include "check.hpp"

namespace tisple_tests {

// all simulator tests: hand-written cases and the SingleStepTests data when present
void RunEmuTests(TestResults& results);

void RunEmuBasicTests(TestResults& results);
void RunSingleStepTests(TestResults& results);

} // namespace tisple_tests
