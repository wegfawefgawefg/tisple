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

// for one seed in kMotionPercent hundred, the seed's program with loop-invariant work and
// constants (fuzz_gen_motion.cpp) among its statements, for constant propagation and code
// motion; "" for the others. A program of its own too
constexpr int kMotionPercent = 25;
std::string GenerateMotionProgram(std::uint64_t seed);

// for one seed in kCommonPercent hundred, the seed's program with work done twice, branches on
// equalities and calls with constant arguments (fuzz_gen_common.cpp) among its statements, for
// common subexpressions, facts from conditions and compile-time evaluation; "" for the others
constexpr int kCommonPercent = 25;
std::string GenerateCommonProgram(std::uint64_t seed);

// for one seed in kCollapsePercent hundred, the seed's program with loops whose passes all do
// the same (or whose last pass decides), compares at the edges of ranges and matches on
// counters (fuzz_gen_collapse.cpp) among its statements, for loop collapse, value ranges and
// unread arguments; "" for the others
constexpr int kCollapsePercent = 25;
std::string GenerateCollapseProgram(std::uint64_t seed);

// for one seed in kValuesPercent hundred, the seed's program with locals stepped by variables
// that hold one of two constants, sets whose ranges decide what follows, helpers called with
// the same constants, elements only some indexes store, (if c (continue)) items and
// (+ x (~ x)) (fuzz_gen_values.cpp) among its statements, for slice 5's ranges; "" for the
// others
constexpr int kValuesPercent = 25;
std::string GenerateValuesProgram(std::uint64_t seed);

// for one seed in kFnPtrPercent hundred, the seed's program with calls through function
// pointers (fuzz_gen_fnptr.cpp) among its statements: tables, fields, arguments, returns, and
// targets that use the callers' globals; "" for the others
constexpr int kFnPtrPercent = 25;
std::string GenerateFnPtrProgram(std::uint64_t seed);

// for one seed in kIrqPercent hundred, the seed's program with an interrupt handler of its own
// (fuzz_gen_irq.cpp), run with interrupts raised early in main; "" for the others
constexpr int kIrqPercent = 25;
std::string GenerateIrqProgram(std::uint64_t seed);

// for one seed in kInlinePercent hundred, the seed's program with calls of small helpers that
// return early, set their parameters or the globals their callers pass, in every place a call
// may stand, and divisions by 0, 1, (| x 1) and the dividend (fuzz_gen_inline.cpp), for
// slice 6's inlining and division rules; "" for the others
constexpr int kInlinePercent = 25;
std::string GenerateInlineProgram(std::uint64_t seed);

// for one seed in kSizePercent hundred, the seed's program with small helpers with loops called
// at many places, heavy small helpers, unrollable loops of every kind, (& (f) 0) and its kin
// around calls with effects (through pointers too), and indexes (+ K (as u16 (+ x c)))
// (fuzz_gen_size.cpp), for the code size budget and two folds; "" for the others
constexpr int kSizePercent = 25;
std::string GenerateSizeProgram(std::uint64_t seed);

// for one seed in kFramesPercent hundred, the seed's program with small helpers whose copies'
// locals share bytes, nested in each other's arguments, needed at once, held across calls of
// a helper that stays a JSR, in loops, in a helper called once (fuzz_gen_frames.cpp), for
// backend/frame_share.hpp; "" for the others
constexpr int kFramesPercent = 25;
std::string GenerateFramesProgram(std::uint64_t seed);

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
