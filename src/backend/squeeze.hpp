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
    // the cycles of a run of the program, about: each kept function's Gen::cost times its runs
    // (both weigh loops as code choices do), and the bank stubs' (kStubCycles a crossing)
    std::uint64_t cycles = 0;
    // the functions in the language card's bank 1 (card_bank.hpp), their bytes, the crossings
    std::vector<std::int32_t> bank1;
    std::uint32_t bank1_bytes = 0;
    std::uint64_t crossings = 0;
};

Squeezed NoteSqueezed(const Backend& be, const std::vector<bool>& kept, const SizeChoices& choices,
                      const std::vector<AsmItem>& runtime);

// "the program did not fit ..., built with a tighter size budget: ...", a warning
Diagnostic SqueezeNote(const Program& program, const Squeezed& usual, const Squeezed& squeezed);

// what code in the card's bank 1 traded against the tighter budget the program would fit
// without it (other: that build's, or nullptr when none fits), a warning; chosen says which
// was taken
Diagnostic Bank1Note(const Program& program, const Squeezed& banked, const Squeezed* other,
                     bool chose_bank1);

// every --no-inline name is a function's; else an error with the names' form, a real one as
// the example (or the one meant: draw/text.start for text.start)
bool CheckNoInline(const Program& program, const std::vector<std::string>& names,
                   std::vector<Diagnostic>& errors);

// the quarter-square multiplies' tables (1 KB) and their routines, about
constexpr std::uint64_t kQuarterSquareBytes = 1024 + 96;

} // namespace tisple
