#pragma once

// Running a compiled program under the profiler:
// - with a main, on the built-in Apple IIe (no rom, no keys) or on the bare simulator,
//   whose i/o page returns the fixed test pattern;
// - without one (a library), every function in turn with arguments from a fixed seed, on
//   the bare simulator, until the cycle budget runs out.

#include "backend/backend.hpp"
#include "gaps/profile.hpp"
#include "race/race.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

struct ProfileRunOptions {
    std::uint64_t max_cycles = 10000000;
    bool bare = false;                  // the bare simulator, not the Apple IIe
    std::vector<RacePatch> patches;     // bare with a main: written just before main runs
    std::uint64_t call_cycles = 500000; // no main: the cap for one call
    std::uint64_t seed = 1;             // no main: the arguments
};

struct ProfileRunResult {
    std::string stop; // how the run ended, "cycle limit", "main returned", ...
    std::uint64_t cycles = 0;
    std::uint64_t calls = 0; // no main: calls made
};

bool ProfileCompiled(const Program& program, const Compiled& compiled,
                     const ProfileRunOptions& options, Profiler& profiler, ProfileRunResult& out,
                     std::string& error);

} // namespace tisple
