#pragma once

// Initial values into a 64 KB memory image through the layout: what the binary holds and
// what the startup copies into zero page.

#include "link/layout.hpp"

#include <vector>

namespace tisple {

// one var's packed initial bytes (Var::init) at its layout address; nothing if it has none
void WriteVarInit(const Program& program, const Layout& layout, std::int32_t var,
                  std::vector<std::uint8_t>& memory);

} // namespace tisple
