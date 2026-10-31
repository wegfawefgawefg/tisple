#pragma once

// Where the link puts the code (link_code.cpp): the free stretches of memory, whole
// functions in the first stretch from the current one on that holds them, then each run of
// functions in one stretch laid out at its address.
//
// A branch may reach another function's code (the peephole's branch over a JMP takes the
// JMP's target, which may be a tail call). In one run that is an ordinary branch; to a run
// in another stretch it becomes the long form, an inverted branch over a JMP (AsmItem::far),
// and the code is placed again with the bigger sizes.

#include "backend/link.hpp"

#include <cstdint>

namespace tisple {

constexpr std::uint32_t kCardStart = 0xD000;
constexpr std::uint32_t kCardEnd = 0x10000;

// with an interrupt handler the startup writes the cpu's vectors at the card's top
inline std::uint32_t CardEnd(const Program& program) {
    return program.handler >= 0 ? kCpuVectors : kCardEnd;
}

std::string HexAddress(std::uint32_t value);

// the free stretches code and data may fill, in the order they are filled: from the origin,
// then below it, then the language card. Text page 1, I/O and every main-memory :address
// or :reserve let are cut out
std::vector<AddressRange> FreeStretches(const Backend& be, const BuildOptions& options,
                                        std::uint32_t data_end, bool card);

// where each span's code goes: the stretch and its first address
struct CodePlace {
    std::size_t stretch = 0;
    std::uint32_t address = 0;
};

std::uint32_t SpanSize(const std::vector<AsmItem>& items, const FunctionSpan& span);

// a run of spans placed first, at the start of the first of some stretches that holds them
// all (aux_link.hpp: the code with_aux runs, at the start of the memory below the origin)
struct EarlySpans {
    std::size_t first = SIZE_MAX;
    std::size_t end = SIZE_MAX;
    std::size_t stretch = SIZE_MAX; // the stretches tried; none: in order with the rest
    std::size_t stretch_end = SIZE_MAX;
};

// spans in order, each whole in the first stretch from the current one on that holds it; the
// last span is followed by the memory frames. False when they do not fit. The spans from
// first_bank1 on are the language card's bank 1 code (card_bank.hpp): they go one after
// another in the last stretch, which no other span uses, and the frames follow the span before.
// early's spans go first, at the start of their stretch, when they fit there
bool PlaceSpans(const std::vector<AsmItem>& items, const std::vector<FunctionSpan>& spans,
                const std::vector<AddressRange>& stretches, std::uint32_t mem_frames,
                std::vector<CodePlace>& places, std::uint32_t& frames_at,
                std::size_t first_bank1 = SIZE_MAX, EarlySpans early = EarlySpans());

// every branch whose target is not a label of its own run (the spans in one stretch) becomes
// far; true when one did, and then the sizes have changed and the spans must be placed again
bool MarkFarBranches(Assembly& as, const std::vector<FunctionSpan>& spans,
                     const std::vector<CodePlace>& places);

// lay out each run of spans in one stretch at its address; branches must stay in their run
bool LayOutRuns(Assembly& as, const std::vector<FunctionSpan>& spans,
                const std::vector<CodePlace>& places, std::vector<AddressRange>& runs,
                std::vector<Diagnostic>& errors);

} // namespace tisple
