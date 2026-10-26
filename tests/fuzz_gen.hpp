#pragma once

// Random valid Tisple programs for differential fuzzing: globals (scalars, arrays, struct
// arrays, a struct), a few functions that call earlier ones, lets, sets, ifs, bounded
// loops, matches, pointers into arrays, I/O at $C0xx. Indices are masked into range, so a
// program never writes outside its data; most divisors are made odd, a few may be zero.

#include <cstdint>
#include <string>

namespace tisple_tests {

// the program for one seed; main is t.main and takes no arguments
std::string GenerateProgram(std::uint64_t seed);

} // namespace tisple_tests
