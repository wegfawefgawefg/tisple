#pragma once

#include "check.hpp"
#include "machine/apple2.hpp"

#include <memory>
#include <vector>

namespace tisple_tests {

// the built-in apple iie: soft switches, running, video
void RunMachineTests(TestResults& results);

void RunMachineIoTests(TestResults& results);
void RunMachineRunTests(TestResults& results);
void RunMachineVideoTests(TestResults& results);
void RunMachineMouseTests(TestResults& results);
void RunMachineAuxTests(TestResults& results);
void RunMachineVideo80Tests(TestResults& results);

// a fresh machine with these bytes loaded at $6000 and ready to start
std::unique_ptr<tisple::Apple2> MachineWithBytes(const std::vector<uint8_t>& bytes);

// a fresh machine with tests/programs/<name> loaded at $6000; null if the file is missing
std::unique_ptr<tisple::Apple2> MachineWithFile(const char* name);

// runs until the program stops (or a generous limit)
tisple::RunOutcome RunToStop(tisple::Apple2& a);

} // namespace tisple_tests
