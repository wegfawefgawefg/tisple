#pragma once

// Startup code and the small runtime routines (multiply, divide), as assembler items.
//
// License: this code is copied into every program Tisple builds, so besides
// GPL-3.0-or-later it is offered under MIT (the runtime exception). It was written for
// Tisple from the textbook algorithms (save/restore loops, shift-and-add multiply,
// restoring division with sign fix-up) and is not derived from any other project's code.
// Keep it that way: never paste routines from GPL sources here, nor into code generation
// that emits fixed sequences into every binary.

#include "backend/gen.hpp"

namespace tisple {

// the routines the program uses (DivS needs DivU), each a run of items with its label
void EmitRuntime(Backend& be, std::vector<AsmItem>& out);
// the tables the routines read (the quarter squares), as data bytes; none if none is used
void EmitRuntimeTables(Backend& be, std::vector<AsmItem>& out);

struct StartupPlan {
    std::int32_t main_sym = -1;  // -1: no main, just set up and return
    std::int32_t save_sym = -1;  // 256-byte buffer for zero page
    std::int32_t image_sym = -1; // the zero-page image in the binary
    std::uint32_t zp_base = 0;   // where the image goes
    std::uint32_t zp_count = 0;
    // explicit-address globals outside the file: byte stores before main
    std::vector<std::pair<std::uint16_t, std::uint8_t>> stores;
    // the I/O bases' patch functions (io_bases.hpp), run once before main
    std::vector<std::int32_t> io_patches;
    std::int32_t ready_sym = -1; // labels the startup defines
    std::int32_t after_sym = -1;
    bool card_ram = false; // part of the program is in the language card: switch its ram in
    // the interrupt handler's entry (irq.hpp), -1 for none: installed at $3FE (and the
    // cpu's vectors in the card's ram) with interrupts off, interrupts on for main; the old
    // vector back at the end, the caller's I flag too
    std::int32_t irq_entry = -1;
    std::int32_t irq_direct = -1;
    std::int32_t irq_saved = -1;
};

// save zero page, write the zero-page image and other initial values, JSR main, restore
// zero page, RTS. With card_ram, the language card's ram (bank 2) is switched in first, for
// reads and writes, and the rom back in before the RTS. With an interrupt handler, all of it
// but main runs with interrupts off
void EmitStartup(Backend& be, const StartupPlan& plan, std::vector<AsmItem>& out);

} // namespace tisple
