#pragma once

// what the fuzzer's two files share (test_backend_fuzz.cpp, test_backend_fuzz_file.cpp)

#include "test_backend.hpp"

#include <cstdint>
#include <vector>

namespace tisple_tests {

std::uint64_t EnvNumber(const char* name, std::uint64_t fallback);

// the interrupts an (irq) program runs with: early in main on both, from the seed; half
// straight to the entry ($FFFE), half through a rom-like STA $45 / JMP ($03FE)
struct IrqPlan {
    bool on = false;
    std::vector<std::uint64_t> cycles;
    std::vector<std::uint64_t> steps;
    bool rom = false;
};

IrqPlan MakeIrqPlan(std::uint64_t seed);
void StartIrqs(Diff& d, const IrqPlan& plan);

// TISPLE_FUZZ_FILE=path: run that one program's main both ways and say how they differ;
// false when the variable is not set
bool DiffFile(TestResults& results);
// the programs of fuzz farm miscompiles, as the generator wrote them then
void RunFarmPrograms(TestResults& results);

} // namespace tisple_tests
