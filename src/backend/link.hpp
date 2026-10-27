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

// whether the language card's ram may hold part of the program: no constant jsr goes into
// $D000-$FFFF and no main-memory :address let reaches $D000, so the rom is not needed there
bool CardAllowed(const Program& program, const Layout& shapes);

// fills out from the generated code in be; card puts the language card's ram in the memory
// map. Errors go to errors (be.errors is aimed there too)
LinkResult LinkProgram(Backend& be, const LinkInput& in, bool card, Compiled& out,
                       std::vector<Diagnostic>& errors);

} // namespace tisple
