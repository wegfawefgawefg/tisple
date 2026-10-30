#pragma once

// Every function's code within the size budget (generate.cpp).

#include "backend/gen.hpp"

#include <string>
#include <utility>
#include <vector>

namespace tisple {

// what one function's code added to the symbols' weights (taken back when it is dropped or
// made again)
using WeightsAdded = std::vector<std::pair<std::int32_t, std::uint64_t>>;

// by function: its code is in the binary. Main and every function its code (after inlining)
// names, and theirs; with no main, or when asked, all of them
std::vector<bool> KeptFunctions(const Backend& be, const BuildOptions& options);

// the code of a program the mid-level passes are done with, made again with fewer copies
// while the copies go over the size budget (rounds generations at most): in be, with the
// functions it keeps and what each added to the symbols' weights. choices: what the budget
// turned down
bool GenerateProgram(Program& program, const BuildOptions& options, int rounds, Backend& be,
                     std::vector<WeightsAdded>& added, std::vector<bool>& kept,
                     SizeChoices& choices, std::vector<Diagnostic>& errors);

// the small functions whose copies at their calls go over the size budget: the mid-level
// copies them nowhere either (its copies are code the budget cannot see). Measured on the
// program as the mid-level passes leave it before they inline, in one generation with every
// copy made
void AddCopiesOverBudget(const Program& settled, const BuildOptions& options,
                         std::vector<std::string>& no_inline);

// generations of the code at most, each with fewer copies than the one before
constexpr int kMaxSizeRounds = 8;

} // namespace tisple
