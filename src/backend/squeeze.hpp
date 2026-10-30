#pragma once

// Room for a program that does not fit (squeeze.cpp, for CompileProgram): what one build
// gave up at its level of squeeze (size_budget.hpp), the note that says what a squeezed
// build traded against the usual one, and the check of --no-inline's names.

#include "backend/gen.hpp"

#include <string>
#include <vector>

namespace tisple {

// one build's code, as the size budget left it
struct Squeezed {
    int squeeze = 0;
    std::vector<bool> kept;  // by function: its code is in the binary (KeptFunctions)
    int loops_kept = 0;      // loops the budget left rolled
    bool quarter = false;    // multiplies by quarter squares
    std::uint32_t bytes = 0; // the functions' code, the runtime routines and tables
};

Squeezed NoteSqueezed(const Backend& be, const std::vector<bool>& kept, const SizeChoices& choices,
                      const std::vector<AsmItem>& runtime);

// "the program did not fit ..., built with a tighter size budget: ...", a warning
Diagnostic SqueezeNote(const Program& program, const Squeezed& usual, const Squeezed& squeezed);

// every --no-inline name is a function's; else an error with the names' form, a real one as
// the example (or the one meant: draw/text.start for text.start)
bool CheckNoInline(const Program& program, const std::vector<std::string>& names,
                   std::vector<Diagnostic>& errors);

// the quarter-square multiplies' tables (1 KB) and their routines, about
constexpr std::uint64_t kQuarterSquareBytes = 1024 + 96;

} // namespace tisple
