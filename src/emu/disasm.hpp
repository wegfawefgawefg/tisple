#pragma once

#include "emu/machine.hpp"

#include <string>

namespace tisple {

// disassembles one instruction at pc straight from memory (no bus side effects), as the
// machine's cpu reads it; length gets the instruction size in bytes. undocumented opcodes
// start with '*'.
std::string Disassemble(const Machine& m, uint16_t pc, int& length);

// true for the 151 documented NMOS opcodes
bool IsDocumentedOpcode(uint8_t opcode);
// the same for either chip: the 65C02's 178 (its undefined opcodes are NOPs, but no compiled
// code has them)
bool IsDocumentedOpcode(uint8_t opcode, Cpu cpu);

} // namespace tisple
