#pragma once

#include "check.hpp"

namespace tisple_tests {

// the redundancy profiler and its findings, on hand-made code and on programs/life
void RunGapsTests(TestResults& results);
// the window superoptimizer: evaluator against the simulator, equivalence, search
void RunSuperoptTests(TestResults& results);

} // namespace tisple_tests
