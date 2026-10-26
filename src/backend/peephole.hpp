#pragma once

// Clean-up over one function's finished items, repeated until nothing changes:
// - a load of a value a register already holds goes, when no branch reads its flags
// - jumps to jumps go straight to the end, a JMP to an RTS becomes the RTS, a jump or
//   branch to the next item goes, a branch over a JMP becomes the opposite branch
// - JSR then RTS becomes JMP (a tail call; frames are static, so nothing to unwind)
// - code after a JMP or RTS that no label reaches goes
// - a store to a byte only this function reads (private_syms: its own locals and
//   temporaries) goes when nothing reads it before the next store or the return
//   (dead_stores.cpp)
// - a CLC or SEC that finds the carry already so goes, and a CLC only the way into a loop
//   needs moves before the loop (carry.cpp)
// Values are only tracked for symbols (never for numbers, which is where I/O lives), and
// any label, call or indexed store forgets what memory holds.

#include "backend/gen.hpp"

namespace tisple {

void Peephole(const Assembly& as, std::vector<AsmItem>& items,
              const std::vector<bool>& private_syms);
// true if any store went
bool DropDeadStores(std::vector<AsmItem>& items, const std::vector<bool>& private_syms);
// true if any CLC or SEC went or moved
bool CarryPass(std::vector<AsmItem>& items);

} // namespace tisple
