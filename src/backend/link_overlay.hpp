#pragma once

// The link's part of the overlay groups (overlay.hpp; link_overlay.cpp): the calls of roots
// routed to their stubs, the groups' images placed in aux memory, and what the simulator needs.

#include "backend/link_code.hpp"
#include "backend/overlay.hpp"

#include <vector>

namespace tisple {

// every call or jump to a root from outside its group made to its stub: the startup, the
// runtime, the interrupt entry and every kept function's code; ov gets the stubs and the copy
void RouteOverlayCode(Backend& be, const LinkInput& in, const OverlayGroups& groups,
                      std::vector<AsmItem>& startup, std::vector<AsmItem>& runtime,
                      std::vector<AsmItem>& irq, OverlayCode& ov);

// each group's functions' code at the end of the items, a span each: the region's spans
RegionSpans AppendGroupSpans(Backend& be, const OverlayGroups& groups, Compiled& out);

// the groups' images one after another in aux memory, next to the lets, consts and mirrored
// code there (aux_reserved); the region's, images' and sizes' symbols defined and
// out.overlays filled in. False (out.overlay_aux_short) when aux memory has no room
bool PlaceGroupImages(Backend& be, const Program& program, const Layout& layout,
                      const std::vector<AddressRange>& aux_reserved, const RegionSpans& region,
                      const OverlayGroups& groups, const OverlayCode& ov, Compiled& out,
                      std::vector<AddressRange>& images, std::vector<Diagnostic>& errors);

// by item: the overlay group whose code it is, or -1
std::vector<std::int32_t> ItemGroups(std::size_t items, const std::vector<FunctionSpan>& spans,
                                     const RegionSpans& region);

// the lets :in aux with no value, of kClearedAuxBytes or more, by var id: in a program that
// runs once with overlay groups the startup clears them (EmitAuxClear) instead of the disk
// loading their zeros, and they go past the rest of aux memory (LayoutOptions::aux_high)
constexpr std::uint32_t kClearedAuxBytes = 0x400;
std::vector<bool> AuxClearedLets(const Program& program);

// the clearing, at label `clear`: each of those lets' bytes zeroed in aux memory (RAMWRT on
// around each, interrupts off), 11 cycles a byte. Its own span, kAuxClearSpan, at the region
constexpr const char* kAuxClearSpan = "aux clearing";
void EmitAuxClear(Backend& be, const std::vector<bool>& cleared, std::int32_t clear,
                  std::vector<AsmItem>& out);

// the overlay copy's writes of $C003 and $C002 and the aux clearing's of $C005 and $C004
// (Compiled::aux_switches)
std::vector<std::uint16_t> OverlayAuxSwitches(const std::vector<AsmItem>& items,
                                              const std::vector<FunctionSpan>& spans);

} // namespace tisple
