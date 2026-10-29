#include "fuzz_gen_internal.hpp"

// wait_cycles and string literals (2026-10-29), from a stream of their own. A wait changes
// nothing the interpreter sees, so the 6502 has to agree with it on every value around one:
// constant waits of the sizes each plan takes (straight, one loop, two), and variable ones
// read where they are or worked out into a temporary first. A string read is a byte of one of
// a few texts, so equal ones share a table, at an index masked to the text and its 0.

namespace tisple_tests::fuzz {

namespace {

// texts 1, 3, 7 and 15 long, so a mask of the length reaches the 0 after them too
const char* const kTexts[] = {"x", "abc", "Hello, ", "Tisple ][ text!"};
const char* const kMasks[] = {"1", "3", "7", "15"};

const int kWaits[] = {2, 3, 4, 5, 7, 9, 14, 30, 47, 100, 255, 256, 1000, 1285, 1290, 1300};

} // namespace

std::string Gen::LateWait() {
    switch (late_rng.Below(4)) {
    case 0:
        return "(wait_cycles " + std::to_string(2 + late_rng.Below(60)) + ")";
    case 1: {
        int n = kWaits[late_rng.Below(16)];
        // a long one only where no loop repeats it
        if (loops == 0 && late_rng.Chance(10)) {
            n = 2000 + late_rng.Below(63000);
        }
        return "(wait_cycles " + std::to_string(n) + ")";
    }
    case 2:
        return "(wait_cycles (as u16 (& " + NonLit(T::U8, 1) + " 31)))";
    default: {
        std::string w = Fresh("w");
        return "(do (let " + w + " u16 (& " + Expr(T::U16, 2) + " 127)) (wait_cycles " + w + "))";
    }
    }
}

std::string Gen::StringRead(int depth) {
    int pick = late_rng.Below(4);
    return std::string("(load (+ \"") + kTexts[pick] + "\" (& " + NonLit(T::U8, depth - 1) + " " +
           kMasks[pick] + ")))";
}

} // namespace tisple_tests::fuzz
