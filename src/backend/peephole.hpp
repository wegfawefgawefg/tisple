#pragma once

// Clean-up over one function's finished items, repeated until nothing changes:
// - a load of a value a register already holds goes, when no branch reads its flags
// - jumps to jumps go straight to the end, a JMP to an RTS becomes the RTS, a jump or
//   branch to the next item goes, a branch over a JMP becomes the opposite branch
// - JSR then RTS becomes JMP (a tail call; frames are static, so nothing to unwind)
// - code after a JMP or RTS that no label reaches goes
// - a store goes when nothing reads the byte before the next store to it: for bytes only
//   this function reads (its own locals, parameters and temporaries, and the locals of the
//   functions it inlines), before the return either; for globals and other functions'
//   parameters (exposed), a call, a jump out or the return reads them all (dead_stores.cpp)
// - a CLC or SEC that finds the carry already so goes, and a CLC only the way into a loop
//   needs moves before the loop (carry.cpp)
// - a load or ALU result whose registers and flags nothing reads goes (dead_regs.cpp)
// Values are only tracked for symbols (never for numbers, which is where I/O lives), and
// any label, call or indexed store forgets what memory holds. A wait item (gen_wait.cpp) is
// never dropped or moved; it keeps the registers and memory (but a variable wait's two scratch
// bytes) and changes the flags.

#include "backend/gen.hpp"

namespace tisple {

// by symbol: what the dead-store pass may assume about who reads its bytes
enum class StoreScope : std::uint8_t {
    Untracked, // pointers may reach it, it has a fixed address (maybe I/O), or not a variable
    Private,   // only this function's code reads it, after writing it
    Exposed,   // read directly by any code: what calls, jumps out and the return may read
};

// frames: by symbol, for a byte of an inlined callee's frame, the callee (else -1). Frames of
// functions that never run at the same time share bytes (frames.cpp), and code with several
// callees inlined holds bytes of each: a write to one callee's byte may change another's.
void Peephole(const Assembly& as, std::vector<AsmItem>& items,
              const std::vector<StoreScope>& scopes, const std::vector<std::int32_t>& frames);
// true if any store went
bool DropDeadStores(std::vector<AsmItem>& items, const std::vector<StoreScope>& scopes);
// true if any CLC or SEC went or moved
bool CarryPass(std::vector<AsmItem>& items);
// true if any load or ALU result nothing reads went (dead_regs.cpp)
bool DropDeadValues(const Assembly& as, std::vector<AsmItem>& items);
// by item: nothing reads N, Z or C before writing them, from there on (dead_regs.cpp)
std::vector<bool> FlagsDeadIn(const Assembly& as, const std::vector<AsmItem>& items);

} // namespace tisple
