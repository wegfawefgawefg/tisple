#pragma once

// Initial values into a 64 KB memory image through the layout: what the binary holds and
// what the startup copies into zero page. Function pointers in const tables get their
// functions' values here too.

#include "link/layout.hpp"

#include <vector>

namespace tisple {

// a function's value with no compiled address (the interpreter alone): never 0, one per
// function
constexpr std::uint16_t kFunctionValueBase = 0xF000;

// what a function's name is worth as a value on this layout
std::uint16_t FunctionValue(const Layout& layout, std::int32_t function);
// the function a function pointer's value names, or -1 when it names none
std::int32_t FunctionOfValue(const Program& program, const Layout& layout, std::uint16_t value);

// what an address slot holds on this layout: a function's value, or the address of a byte of a
// var (stored packed, as its address is taken)
std::uint16_t SlotValue(const Layout& layout, const AddrSlot& slot);

// a var's packed initial bytes with its address slots (Var::addr_slots) filled in
std::vector<std::uint8_t> InitBytes(const Program& program, const Layout& layout, std::int32_t var);

// one var's packed initial bytes (InitBytes) at its layout address; nothing if it has none
void WriteVarInit(const Program& program, const Layout& layout, std::int32_t var,
                  std::vector<std::uint8_t>& memory);

} // namespace tisple
