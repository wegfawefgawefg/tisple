#pragma once

// The interrupt handler's entry (docs/design.md, "Interrupt handlers"). The handler itself is
// an ordinary function; what the IRQ vector reaches is this entry:
//
//   irq.direct:  STA $45        $FFFE in the card's ram, when the program runs with it in
//   irq.entry:   CLD            $3FE, where the ROM's IRQ code jumps: A is in $45 (II+,
//                TXA, PHA       unenhanced IIe) or on the stack (enhanced IIe, which also
//                TYA, PHA       restores it after the RTI)
//                LDA zp, PHA    each zero-page scratch byte the handler's code may use
//                JSR handler
//                PLA, STA zp    the same, the other way
//                PLA, TAY, PLA, TAX
//                LDA $45
//                RTI
//
// The handler's functions get frames nothing else shares (frames.cpp), so the scratch bytes
// are all it can share with the code it stops; $45 is kept out of zero page.

#include "backend/gen.hpp"

namespace tisple {

// the zero-page byte the ROM's IRQ code saves A in (II+, unenhanced IIe)
constexpr std::uint16_t kIrqSaveA = 0x45;
// the IRQ vector the ROM jumps through, and the cpu's in the language card's ram
constexpr std::uint16_t kIrqVector = 0x03FE;
constexpr std::uint16_t kCpuVectors = 0xFFFA;

struct IrqPlan {
    std::int32_t entry_sym = -1;  // irq.entry
    std::int32_t direct_sym = -1; // irq.direct
    std::int32_t saved_sym = -1;  // 2 bytes: the $3FE vector the startup found
};

// by function: run by the interrupt handler (none without one)
std::vector<bool> HandlerFunctions(const Program& program);

// the entry's items, saving what the kept code of the handler's functions (and the runtime
// routines it calls) may use of the scratch bytes; the plan's labels
void EmitIrqEntry(Backend& be, const std::vector<bool>& kept, const std::vector<AsmItem>& runtime,
                  IrqPlan& plan, std::vector<AsmItem>& out);

} // namespace tisple
