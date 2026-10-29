#pragma once

// tisple race-gen: the random-program race's programs (races/random.py). For each seed the
// fuzz generator's race-mode program (tests/fuzz_gen.hpp GenerateRaceProgram) as t.tsp,
// the same program in C as t.c (race_c.hpp), the interpreter's checksum, and a crude shape
// signature: which language features the timed code uses, and how often.

#include "check/program.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tisple {

using RaceTextFn = std::string (*)(std::uint64_t seed);

// the features of t.run and everything it calls, by name: "* u8", "for u16", "call", ...
// With counts (Interp::expr_counts after a run), each feature weighs as often as its exprs
// ran: where the program's work is, not just what it says
std::map<std::string, std::uint64_t> RaceFeatures(const Program& program,
                                                  const std::vector<std::uint32_t>* counts);

int RunRaceGenCommand(int argc, char** argv, RaceTextFn generate);

} // namespace tisple
