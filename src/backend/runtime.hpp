#pragma once

// Startup code and the small runtime routines (multiply, divide), as assembler items.

#include "backend/gen.hpp"

namespace tisple {

// the routines the program uses (DivS needs DivU), each a run of items with its label
void EmitRuntime(Backend& be, std::vector<AsmItem>& out);

struct StartupPlan {
    std::int32_t main_sym = -1;  // -1: no main, just set up and return
    std::int32_t save_sym = -1;  // 256-byte buffer for zero page
    std::int32_t image_sym = -1; // the zero-page image in the binary
    std::uint32_t zp_base = 0;   // where the image goes
    std::uint32_t zp_count = 0;
    // explicit-address globals outside the file: byte stores before main
    std::vector<std::pair<std::uint16_t, std::uint8_t>> stores;
    std::int32_t ready_sym = -1; // labels the startup defines
    std::int32_t after_sym = -1;
};

// save zero page, write the zero-page image and other initial values, JSR main, restore
// zero page, RTS
void EmitStartup(Backend& be, const StartupPlan& plan, std::vector<AsmItem>& out);

} // namespace tisple
