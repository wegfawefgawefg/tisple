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

// the overlay copy's writes of $C003 and $C002 (Compiled::aux_switches)
std::vector<std::uint16_t> OverlayAuxSwitches(const std::vector<AsmItem>& items,
                                              const std::vector<FunctionSpan>& spans);

} // namespace tisple
