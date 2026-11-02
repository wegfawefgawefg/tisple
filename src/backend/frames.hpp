#pragma once

// Static frames and zero page, for the whole program at once.
//
// Every function's frame holds its params, locals and temporaries. A frame starts after the
// frames of every function that can call it (directly or not), so functions that are never
// live together share bytes. There are two such stacks: one in zero page, one in memory. The
// interrupt handler's functions share none with the rest: an IRQ comes at any time.
//
// The locals the mid-level passes make (inlined copies' above all) share slots by where the
// finished code holds their values, a copy's in the frame of the function it was copied from
// (frame_share.hpp); a made local no code names gets no bytes. Inside a frame the program's
// variables come first, then what only the code uses, and over that the variables no code
// names (only the interpreter, which runs the program as written on this layout, reads them).
//
// Zero page gets, in order: the runtime scratch bytes, every dereferenced pointer and
// pointer temporary (forced), then frame items and small globals by uses per byte (each
// use counts 4x per enclosing loop) while they fit; globals first in the second longest free
// run, which frames cannot use (they need the one stack). Explicit :address placements in zero
// page are left alone.

#include "backend/gen.hpp"

namespace tisple {

struct FramePlan {
    std::uint32_t zp_used = 0;
    std::uint32_t zp_frames = 0; // size of the zero-page frame stack
    std::uint32_t mem_frames = 0;
    std::uint32_t zp_globals_base = 0;
    std::uint32_t zp_globals_size = 0;
    std::vector<std::int32_t> zp_globals; // vars, in address order
    // globals in the second longest free run of zero page (the first holds the frames too;
    // with an interrupt handler $45 splits zero page in two): their own image at startup
    std::uint32_t zp2_globals_base = 0;
    std::uint32_t zp2_globals_size = 0;
    // frame items outside zero page: symbol and offset from the memory-frame base
    std::vector<std::pair<std::int32_t, std::uint32_t>> mem_items;
};

// defines every zero-page symbol (scratch, frames, globals); memory frames get offsets
bool AllocateFrames(Backend& be, FramePlan& plan);

// functions in an order where every caller comes before its callees
std::vector<std::int32_t> CallersFirst(const Program& program);

} // namespace tisple
