#pragma once

#include "backend/backend.hpp"
#include "backend/sim_run.hpp"
#include "check.hpp"
#include "interp/interp.hpp"
#include "test_check.hpp"

#include <string>
#include <vector>

namespace tisple_tests {

// a checked program compiled by the backend; the interpreter runs on the same layout
struct Compiled {
    Built built;
    tisple::Compiled code;
    bool ok = false;
    std::string errors;
};

Compiled CompileText(const std::string& text, const std::vector<tisple::VirtualFile>& extra = {});
Compiled CompileFile(const std::string& path);

// the interpreter and the simulator side by side, calls made on both
struct Diff {
    const Compiled* c = nullptr;
    tisple::Interp in;
    std::vector<tisple::IoAccess> interp_io;
    tisple::SimRunner sim;
    std::uint64_t max_cycles = 50000000;
    std::uint64_t last_cycles = 0;
    std::string message; // the first difference
};

bool StartDiff(Diff& d, const Compiled& c);
// calls on both, then compares the return value, every global byte and the I/O accesses
bool DiffCall(Diff& d, const std::string& function, const std::vector<std::uint16_t>& args);
// the same, checked: a failure prints the program
void CheckDiff(TestResults& results, Diff& d, const std::string& function,
               const std::vector<std::uint16_t>& args);
// compile, start and call one function; the text is printed on any failure
void CheckProgram(TestResults& results, const std::string& text, const std::string& function,
                  const std::vector<std::uint16_t>& args);

// instructions of one mnemonic in a function's code
int CountMn(const Compiled& c, const std::string& function, tisple::Mn mn);

// compares every global byte; false with a message
bool SameGlobals(const Diff& d, std::string& message);

void RunAsmTests(TestResults& results);
void RunBackendTests(TestResults& results);
void RunBackendOpTests(TestResults& results);
void RunBackendProgramTests(TestResults& results);
void RunBackendIndexTests(TestResults& results);
void RunBackendFuzzTests(TestResults& results);
void RunBackendDivTests(TestResults& results);
void RunBackendCarryTests(TestResults& results);
void RunBackendReduceTests(TestResults& results);

} // namespace tisple_tests
