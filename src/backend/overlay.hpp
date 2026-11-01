#pragma once

// Overlay groups in the generated code (docs/overlays.md): code that takes turns in one region
// of main memory or the language card, each group's bytes kept in aux memory and copied in when
// one of its roots is called and another group is in.
//
// - The groups come from the plan (overlay_plan.hpp), found again in the kept code: a group is
//   its roots and every kept function reached only through them, by the code's own references
//   (calls, tail jumps, branches) and the program's calls (for pointer calls). Never in one: what
//   the interrupt handler runs, a dispatcher or a pointer call's entry, an I/O base's patch
//   function, code with an aux switch on. A group any of whose labels other code names but by
//   calling or jumping to a root's entry, or that names another group's, is left out.
// - Every call or jump to a root from outside its group goes through the root's stub, in main
//   memory after the startup:
//
//     ov.r:       JMP ov.r.load         JMP r while r's group is in (3 cycles a call)
//     ov.r.load:  JSR ov.g.enter / JMP r
//
//   and per group:
//
//     ov.g.enter: PHP / PHA / TXA / PHA / TYA / PHA      the call's registers and flags
//                 JSR ov.h.off for every other group h    their stubs back to their loads
//                 the image's address, the region's, its pages and the bytes after
//                 JSR ov.copy
//                 each of g's stubs to JMP its root
//                 PLA / TAY / PLA / TAX / PLA / PLP / RTS
//
//   ov.copy copies whole pages (LDA (rt_a),Y / STA (rt_b),Y, 16 cycles a byte) and then the
//   bytes left, each page with RAMRD on and interrupts off (PHP / SEI ... PLP): a tick waits at
//   most a page's 4,100 cycles, and the handler never runs with RAMRD on. It is the one piece
//   that runs with RAMRD on, so its code is in both memories (aux_link.hpp's mirrored code).
// - The region is the biggest group's bytes, after the other code in the first stretch that holds
//   it; the frames in memory follow it. Groups' frames share bytes as any functions' do: groups
//   that take turns never run together.

#include "backend/gen.hpp"
#include "backend/overlay_plan.hpp"
#include "link/layout.hpp"

#include <vector>

namespace tisple {

struct OverlayGroups {
    std::vector<std::int32_t> group;                // by function: its group, -1 when resident
    std::vector<std::vector<std::int32_t>> roots;   // by group: its roots that have code
    std::vector<std::vector<std::int32_t>> members; // by group: its functions, roots first
    std::vector<std::uint32_t> bytes;               // by group: its code, before far branches
    bool Any() const {
        return !roots.empty();
    }
};

// the names of the link's spans of them (FunctionSpan::name)
constexpr const char* kOverlayStubsSpan = "overlay stubs";
constexpr const char* kOverlayCopySpan = "overlay copy";

// the plan's groups in the kept code; those that cannot be overlaid as planned left out
OverlayGroups FindOverlayGroups(const Backend& be, const std::vector<bool>& kept,
                                const OverlayPlan& plan, const std::vector<AsmItem>& startup,
                                const std::vector<AsmItem>& runtime);

// what the link makes for the groups: the stubs and enter routines (resident), the copy
// (mirrored), and the symbols it fills in once the region and the images have addresses
struct OverlayCode {
    std::vector<AsmItem> stubs;          // ov.r, ov.r.load, ov.g.enter, ov.g.off
    std::vector<AsmItem> copy;           // ov.copy
    std::vector<std::int32_t> stub;      // by function: its stub's label (roots), else -1
    std::vector<std::int32_t> image_sym; // by group: its bytes in aux memory
    std::vector<std::int32_t> size_sym;  // by group: whole pages (low) and bytes after (high)
    std::int32_t region_sym = -1;
};

// the stubs and routines; every call or jump to a root from outside its group in pieces (all
// code but the groups' own, and each group's code, pieces[i] in group piece_group[i] or -1)
// made to its stub
void RouteOverlays(Backend& be, const OverlayGroups& groups,
                   const std::vector<std::vector<AsmItem>*>& pieces,
                   const std::vector<std::int32_t>& piece_group, OverlayCode& code);

// a free stretch of aux memory for the images, size bytes, next to what is there (so it joins
// a segment): taken is every range in use. False when none holds them
bool PlaceOverlayImages(const std::vector<AddressRange>& taken, std::uint32_t size,
                        std::uint32_t& at);

// a load's cycles, about: the copy and the stubs' patching
std::uint64_t OverlayLoadCycles(std::uint32_t bytes, std::size_t stubs);

} // namespace tisple
