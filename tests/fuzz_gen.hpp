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

// for one seed in kEntryPercent hundred, the seed's program with loops entered by a jump
// (fuzz_gen_entries.cpp) among its statements; "" for the others. A second program for those
// seeds, so GenerateProgram's programs stay what they were
constexpr int kEntryPercent = 25;
std::string GenerateEntryProgram(std::uint64_t seed);

// the same for waits and string literals (fuzz_gen_late.cpp), for one seed in kLatePercent
// hundred; "" for the others
constexpr int kLatePercent = 25;
std::string GenerateLateProgram(std::uint64_t seed);
// for one seed in kDeadPercent hundred, the seed's program with dead work and work that only
// looks dead (fuzz_gen_dead.cpp) among its statements, for the mid-level passes; "" for the
// others. A program of its own too
constexpr int kDeadPercent = 25;
std::string GenerateDeadProgram(std::uint64_t seed);

// the seed's program for the random-program race (races/random.py): no I/O, start values set
// by t.init instead of initializers, the generated main renamed t.run, then a main that calls
// init, run and t.checksum, which folds every global (and run's value, in rv) into a u16 at
// kRaceChecksumAddress. src/race/race_c.hpp writes the same program in C. Outside race mode
// nothing changes: GenerateProgram's text for a seed stays what it was
std::string GenerateRaceProgram(std::uint64_t seed);
constexpr std::uint16_t kRaceChecksumAddress = 0xBF00;
// t.init adds the u16 here to every start value; the race writes it as 0 at main's entry
constexpr std::uint16_t kRaceOpaqueAddress = 0xBC00;

} // namespace tisple_tests
