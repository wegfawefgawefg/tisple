#pragma once

// one piece of a program's bytes and the memory it loads into. A program that fits one region
// is one main segment at its origin; a bigger one also has pieces in other regions of main
// ram, in the language card and in auxiliary ram (docs/language.md, Memory map).

#include <cstdint>
#include <vector>

namespace tisple {

enum class MemoryBank : std::uint8_t {
    kMain, // main ram, $0200-$BFFF
    kCard, // the main language card's ram, $D000-$FFFF with bank 2 at $D000
    kAux,  // auxiliary ram, $0200-$BFFF
    // the main language card's bank 1, $D000-$DFFF: code the program switches in with $C08B
    // (backend/card_bank.hpp)
    kCardBank1,
};

struct MemorySegment {
    MemoryBank bank = MemoryBank::kMain;
    std::uint16_t address = 0;
    std::vector<std::uint8_t> bytes;
};

inline const char* MemoryBankName(MemoryBank bank) {
    switch (bank) {
    case MemoryBank::kCard:
        return "language card";
    case MemoryBank::kAux:
        return "aux";
    case MemoryBank::kCardBank1:
        return "language card bank 1";
    default:
        return "main";
    }
}

} // namespace tisple
