#pragma once

// The 65C02's instructions in finished code (BuildOptions::cpu, compile.cpp): one pass over a
// function's items (and the runtime routines') after the peephole and the frame allocation,
// so every analysis before it reads NMOS code only. Each rewrite costs no more cycles or bytes
// than what it replaces:
// - a store of a register known to hold 0 becomes STZ (zp, zp,X, abs, abs,X); the load of the
//   0 then goes when nothing else reads it (dead_regs.cpp)
// - (zp),Y with Y known to hold 0 becomes (zp): a cycle less for STA, no page-crossing cycle
//   for the others; the LDY #0 then goes the same way
// - CLC/ADC #1 and SEC/SBC #1 (and their #$FF) become INC A or DEC A when nothing reads C or V
//   after: 2 bytes and 2 cycles less
// - TXA/PHA and TYA/PHA become PHX and PHY when nothing reads A, N or Z after; PLA/TAX and
//   PLA/TAY become PLX and PLY when nothing reads A after
// - LDA m / ORA #k / STA m becomes LDA #k / TSB m, and with AND #k, LDA #~k / TRB m, for RAM
//   the program owns (TSB reads twice) when nothing reads A, N or Z after
// "Known to hold 0" is straight-line knowledge: from a load of #0 (or a transfer of a register
// that holds it) to the next label, call or change of the register.
//
// The jump tables (gen_match.cpp: JMP (table,X)), the dispatchers (dispatch.cpp: JMP (pointer)),
// the interrupt entry (irq.cpp: PHX/PHY) and the assembler (BRA for near JMPs) use the 65C02
// where they are made.

#include "backend/gen.hpp"

namespace tisple {

// true when anything changed
bool UseCmos(const Assembly& as, std::vector<AsmItem>& items);

} // namespace tisple
