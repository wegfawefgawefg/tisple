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
// nodes of a kind under a function's body after the passes; -1 for no such function
int CountKind(const Opted& o, const std::string& function, tisple::ExprKind kind);
void ExpectCount(TestResults& results, const std::string& text, const std::string& function,
                 tisple::ExprKind kind, int want);

void RunOptTests(TestResults& results);
// -O0, --opt-bisect and --no-pass (test_opt_flags.cpp)
void RunOptFlagTests(TestResults& results);

} // namespace tisple_tests
