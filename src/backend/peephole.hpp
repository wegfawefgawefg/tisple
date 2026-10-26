#pragma once

// Clean-up over one function's finished items, repeated until nothing changes:
// - a load of a value a register already holds goes, when no branch reads its flags
// - jumps to jumps go straight to the end, a JMP to an RTS becomes the RTS, a jump or
//   branch to the next item goes, a branch over a JMP becomes the opposite branch
// - JSR then RTS becomes JMP (a tail call; frames are static, so nothing to unwind)
// - code after a JMP or RTS that no label reaches goes
// Values are only tracked for symbols (never for numbers, which is where I/O lives), and
// any label, call or indexed store forgets what memory holds.

#include "backend/gen.hpp"

namespace tisple {

void Peephole(const Assembly& as, std::vector<AsmItem>& items);

} // namespace tisple
