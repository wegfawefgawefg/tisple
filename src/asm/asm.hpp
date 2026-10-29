#pragma once

// A small 6502 assembler with symbols, fixups and branch relaxation.
//
// - Items are instructions, labels, single data bytes and notes (source lines for the
//   listing), in address order.
// - Operands are a number or a symbol plus an offset, optionally its low or high byte.
//   Every symbol is defined before encoding; labels define themselves in LayoutCode.
// - Abs becomes Zp when the operand is known to live in zero page: a symbol marked zp, or a
//   number below $100. Indexed modes are never shrunk, since zp,X wraps inside page zero
//   while abs,X does not.
// - A branch whose target is out of range becomes the inverted branch over a JMP.
// - A wait (asm/wait.hpp) is one item whose bytes depend on its address and whose size does
//   not: an exact delay that keeps the registers and memory and may change the flags.

#include "asm/opcodes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

enum class Part : std::uint8_t {
    Whole,
    Low,
    High,
};

struct AsmValue {
    std::int32_t sym = -1; // -1: just the number in offset
    std::int32_t offset = 0;
    Part part = Part::Whole;
};

enum class ItemKind : std::uint8_t {
    Ins,
    Label, // value.sym is the label
    Byte,  // one data byte: the value's low byte (or the part asked for)
    Note,  // listing text in Assembly::notes[note]
    // an exact delay: mode Imp, value the cycles (a number); or mode Abs, a variable wait of
    // the u16 at value (kVarWaitMin at least), counting in the zero-page bytes at `scratch`
    Wait,
};

// what code generation knows about the carry an instruction leaves, beyond what the
// instruction alone shows: after an add that cannot carry, or when a branch is taken
enum class Carry : std::uint8_t {
    Unknown,
    Clear,
    Set,
};

struct AsmItem {
    ItemKind kind = ItemKind::Ins;
    Mn mn = Mn::NOP;
    Mode mode = Mode::Imp;
    AsmValue value;
    std::int32_t note = -1;
    Carry carry = Carry::Unknown; // for a branch: when taken; else after it
    AsmValue scratch;             // a variable wait's counter
    // set by LayoutCode
    std::uint16_t address = 0;
    std::uint8_t size = 0;
    bool long_branch = false;
};

struct AsmSymbol {
    std::string name;
    bool zp = false; // lives in zero page; known before LayoutCode
    bool defined = false;
    std::uint32_t value = 0;
};

struct Assembly {
    std::vector<AsmSymbol> symbols;
    std::vector<AsmItem> items;
    std::vector<std::string> notes;
};

std::int32_t NewSymbol(Assembly& as, const std::string& name);
void DefineSymbol(Assembly& as, std::int32_t sym, std::uint32_t value);

AsmItem MakeIns(Mn mn, Mode mode, AsmValue value = AsmValue());
AsmItem MakeLabel(std::int32_t sym);
AsmItem MakeByte(AsmValue value);
AsmItem MakeWait(std::uint32_t cycles);
AsmItem MakeVarWait(AsmValue low, AsmValue scratch);
AsmValue Num(std::int32_t number);
AsmValue SymValue(std::int32_t sym, std::int32_t offset = 0, Part part = Part::Whole);

// the mode an instruction is encoded with (Abs may become Zp)
Mode EncodedMode(const Assembly& as, const AsmItem& item);

// sizes and addresses from origin; defines labels; relaxes branches. Returns the end address
// (one past the last byte), which may be past $FFFF if the code does not fit.
std::uint32_t LayoutCode(Assembly& as, std::uint16_t origin);

// bytes from origin to the end; every symbol must be defined. False with a message on a
// missing symbol or an operand that does not fit its mode.
bool EncodeCode(const Assembly& as, std::vector<std::uint8_t>& out, std::string& error);

// one item as listing text, "LDA $80" (with "  ; name+1" for symbols); bytes from EncodeCode
std::string ItemText(const Assembly& as, const AsmItem& item);
// cycles of an instruction, not counting page crossings or taken branches; a wait's exact
// count (a variable one's overhead)
int ItemCycles(const Assembly& as, const AsmItem& item);

} // namespace tisple
