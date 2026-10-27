#pragma once

// The link stage of CompileProgram (link.cpp): startup, code placement, the layout of every
// variable, and the program's bytes as segments.

#include "backend/frames.hpp"

namespace tisple {

struct LinkInput {
    const BuildOptions* options = nullptr;
    const FramePlan* plan = nullptr;
    const std::vector<bool>* kept = nullptr; // by function: its code is in the program
    const std::vector<AsmItem>* runtime = nullptr;
};

enum class LinkResult : std::uint8_t {
    kOk,
    kNoRoom, // the code or the data did not fit; with the language card it might
    kError,
};

// whether the language card's ram may hold part of the program: no jsr, io_read, io_write
// or (as (ptr t) n) has a constant address in $D000-$FFFF and no main-memory :address let
// reaches $D000, so the rom is not needed there
bool CardAllowed(const Program& program, const Layout& shapes);

// fills out from the generated code in be; card puts the language card's ram in the memory
// map. Errors go to errors (be.errors is aimed there too)
LinkResult LinkProgram(Backend& be, const LinkInput& in, bool card, Compiled& out,
                       std::vector<Diagnostic>& errors);

// segments.cpp: the bytes [low, high) of a 64 KB memory image as a segment
MemorySegment SegmentOf(const std::vector<std::uint8_t>& memory, MemoryBank bank, std::uint32_t low,
                        std::uint32_t high);
// every :in aux let's bytes as segments: runs of them, a new one after a gap of a page or more
void AuxSegments(const Program& program, const Layout& layout, const std::vector<std::uint8_t>& aux,
                 std::vector<MemorySegment>& out);

} // namespace tisple
