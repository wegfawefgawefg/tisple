#pragma once

#include "check.hpp"
#include "check/program.hpp"
#include "opt/opt.hpp"

#include <string>

namespace tisple_tests {

// a program checked, its constants folded and the mid-level passes run, as the backend does
struct Opted {
    tisple::Program program;
    tisple::OptReport report;
    bool ok = false;
};

Opted Optimize(const std::string& text, const tisple::OptOptions& options = {});
// every pass but unroll: for tests of what another pass leaves of a loop
tisple::OptOptions NoUnroll();
// nodes of a kind under a function's body after the passes; -1 for no such function
int CountKind(const Opted& o, const std::string& function, tisple::ExprKind kind);
void ExpectCount(TestResults& results, const std::string& text, const std::string& function,
                 tisple::ExprKind kind, int want, const tisple::OptOptions& options = {});

// how deep in loops the deepest node of a kind (and op, for a binary) sits; 0: in no loop,
// -1: no such node
int LoopDepth(const Opted& o, const std::string& function, tisple::ExprKind kind, int op = -1);

void RunOptTests(TestResults& results);
// constant propagation (test_opt_consts.cpp) and loop-invariant code motion (test_opt_licm.cpp)
void RunOptConstsTests(TestResults& results);
void RunOptLicmTests(TestResults& results);
// common subexpressions (test_opt_cse.cpp) and compile-time evaluation (test_opt_eval.cpp)
void RunOptCseTests(TestResults& results);
void RunOptEvalTests(TestResults& results);
// loop collapse and unread arguments (test_opt_collapse.cpp), value ranges (test_opt_ranges.cpp)
void RunOptCollapseTests(TestResults& results);
void RunOptRangesTests(TestResults& results);
// slice 5's variable ranges, sets' ranges and unstored elements (test_opt_values.cpp)
void RunOptValuesTests(TestResults& results);
// -O0, --opt-bisect and --no-pass (test_opt_flags.cpp)
void RunOptFlagTests(TestResults& results);

} // namespace tisple_tests
