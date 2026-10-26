#pragma once

// Where an address of compiled code came from: its assembly item, function and the source
// line the listing shows above it. Shared by the profiler's report and the superoptimizer.

#include "backend/backend.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

struct SiteInfo {
    std::int32_t item = -1; // into Compiled::as.items; -1 when no instruction is here
    std::int32_t span = -1; // into Compiled::spans
    std::int32_t note = -1; // the source line above it, into Assembly::notes
    std::int32_t prev = -1; // the instruction before it with no label between; -1 at a label
};

struct SiteMap {
    std::vector<SiteInfo> at = std::vector<SiteInfo>(65536); // by address
};

void BuildSiteMap(const Compiled& compiled, SiteMap& out);

// "main.step"; "?" outside every span
std::string SiteFunction(const Compiled& compiled, const SiteMap& map, std::uint16_t address);
// "programs/life/main.tsp:54: (let n u8", without the folders; "" with no note above
std::string SiteSource(const Compiled& compiled, const SiteMap& map, std::uint16_t address);
// the instruction as the listing prints it, "ADC ($10),Y  ; main.step.above"
std::string SiteText(const Compiled& compiled, const SiteMap& map, std::uint16_t address);
// mnemonic and addressing mode without the operand, "ADC (zp),Y", for grouping
std::string ShapeText(Mn mn, Mode mode);
// the shape of the instruction at an item; "label" for -1
std::string ItemShape(const Compiled& compiled, std::int32_t item);

} // namespace tisple
