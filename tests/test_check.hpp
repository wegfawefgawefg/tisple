#pragma once

#include "check.hpp"
#include "check/check.hpp"
#include "interp/interp.hpp"
#include "link/layout.hpp"

#include <string>
#include <vector>

namespace tisple_tests {

// a checked and laid-out program, or the errors that stopped it
struct Built {
    tisple::Program program;
    tisple::Layout layout;
    std::vector<tisple::Diagnostic> errors;
    bool ok = false;
};

// t.tsp holds text; extra files are found by path (lib/ is "lib0")
Built BuildText(const std::string& text, const std::vector<tisple::VirtualFile>& extra = {});
Built BuildFile(const std::string& path);
std::string ErrorText(const Built& built); // every diagnostic, one per line
std::string FirstError(const Built& built);

// checks a program fails with exactly this first error
void ExpectError(TestResults& results, const std::string& text, const std::string& expected,
                 const std::vector<tisple::VirtualFile>& extra = {});

// a fresh interpreter over a built program
void StartInterp(tisple::Interp& in, const Built& built);
std::uint16_t CallOk(TestResults& results, tisple::Interp& in, const std::string& function,
                     const std::vector<std::uint16_t>& args);

void RunCheckExampleTests(TestResults& results);
void RunCheckNameTests(TestResults& results);
void RunCheckTypeTests(TestResults& results);
void RunCheckFormTests(TestResults& results);
void RunExpandTests(TestResults& results);
void RunLayoutTests(TestResults& results);
void RunInterpTests(TestResults& results);
void RunInterpArithTests(TestResults& results);

} // namespace tisple_tests
