#pragma once

// Code in the language card's bank 1 (docs/overlays.md), for a program that does not fit main
// memory and the card as the startup leaves it (bank 2 at $D000).
//
// - The card's $D000-$DFFF is two 4 KB banks; a read of $C08B puts bank 1 in, one of $C083
//   bank 2 (the startup's two reads of $C083 enabled writes, and odd reads keep them on).
//   $E000-$FFFF and main memory are the same whichever is in.
// - The rule: bank 1 is in only while code in it runs. Every call or jump from bank 1 code to
//   anything else goes through a back stub, and every call or jump into it from anything else
//   through a forward stub; both live in main memory (just after the startup):
//
//     bank1.f:  BIT $C08B / JSR f / BIT $C083 / RTS    calls into bank 1
//     bank2.g:  BIT $C083 / JSR g / BIT $C08B / RTS    calls out of it
//
//   20 cycles a crossing, 10 bytes a stub. BIT keeps A, X, Y and the carry; the N, V and Z
//   it changes are never read after a call (our code returns values in registers). A tail
//   jump through a stub is a call and a return, the same thing. Calls between bank 1
//   functions are plain JSRs.
// - What may go: a kept function no interrupt handler reaches, not a dispatcher or a pointer
//   call's entry (the pointer is the entry's address, so those stay put and call through
//   stubs), not an I/O base's patch function, no JSR or JMP to an address or through one, no
//   operand in $D000-$DFFF, and no label another function's code jumps into, reads, writes or
//   takes the address of (and the other way round) but its entry, called.
// - Data: bank 2's $D000-$DFFF keeps holding data and code; the link keeps out of it every
//   variable the bank 1 functions' code names, and every variable whose address is taken when
//   one of them reads or writes through a pointer (LayoutOptions::avoiders), and the frames
//   in memory.
// - The interrupt entry (irq.hpp) saves $C011, puts bank 2 in, and puts bank 1 back after
//   the handler when it was in, so the handler and its data may be anywhere but bank 1.
// - Which functions: those with the fewest crossings for their bytes first (calls into them
//   by FindRunCounts, calls out of them by FindCallWeights, multiplies by their runs), as many
//   as the program needs to fit (CompileProgram).

#include "backend/gen.hpp"

namespace tisple {

constexpr std::uint32_t kBank1Start = 0xD000;
constexpr std::uint32_t kBank1End = 0xE000;
constexpr std::uint16_t kBank1Switch = 0xC08B; // a read: bank 1 at $D000
constexpr std::uint16_t kBank2Switch = 0xC083; // a read: bank 2 at $D000
constexpr std::uint16_t kBankStatus = 0xC011;  // bit 7: bank 2 is in
constexpr std::uint64_t kStubCycles = 20;      // a crossing's cycles over a plain JSR and RTS

// what choosing bank 1 functions needs, from the generated code
struct Bank1Facts {
    // the functions that may go in bank 1, the fewest crossings for their bytes first
    std::vector<std::int32_t> order;
    std::vector<std::uint32_t> bytes;     // by function: its code
    std::vector<std::uint64_t> crossings; // by function: calls in and out per run, alone there
    std::vector<std::vector<std::int32_t>> vars; // by function: the vars its code names
    std::vector<bool> indirect;                  // by function: reads or writes through a pointer
};

Bank1Facts FindBank1Facts(const Backend& be, const std::vector<bool>& kept,
                          const std::vector<AsmItem>& runtime);

// the functions in bank 1, and by var what must stay out of $D000-$DFFF
struct Bank1Plan {
    std::vector<bool> functions;
    std::vector<bool> avoiders;
    std::uint32_t bytes = 0;
    std::uint64_t crossings = 0; // per run of the program, about
    bool all_stubs = false;      // a forward stub for every one (tests call them by address)
};

// the first `count` of facts.order
Bank1Plan PlanBank1(const Backend& be, const Bank1Facts& facts, std::size_t count);

// tests (BuildOptions::bank1_test): the functions of facts.order that `pick` takes, all of
// them for 1, a seeded half otherwise
Bank1Plan PlanBank1Test(const Backend& be, const Bank1Facts& facts, int pick);

// every call or jump from a piece of code in one bank to a label in a piece of the other
// made through a stub; pieces[i] is in bank 1 when bank1[i]. The stubs are appended to
// stubs, and forward[f] is f's forward stub symbol (or -1) by function
void RouteBankCalls(Backend& be, const std::vector<std::vector<AsmItem>*>& pieces,
                    const std::vector<bool>& bank1, const Bank1Plan& plan,
                    std::vector<AsmItem>& stubs, std::vector<std::int32_t>& forward);

// RouteBankCalls over the whole program: the startup, every kept function's code, the runtime
// and the interrupt entry
void RouteBank1(Backend& be, const Bank1Plan& plan, const std::vector<bool>& kept,
                std::vector<AsmItem>& startup, std::vector<AsmItem>& runtime,
                std::vector<AsmItem>& irq, std::vector<AsmItem>& stubs,
                std::vector<std::int32_t>& forward);

} // namespace tisple
