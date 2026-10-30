#pragma once

// What frame_share.cpp learns from the finished code of each function: where the bytes of
// made locals and of other functions' frames hold values (liveness over the items, backwards
// until it settles), and so which of them meet, what is called while a made local holds a
// value, and which made locals the liveness cannot follow.

#include "backend/gen.hpp"

#include <map>
#include <set>

namespace tisple {

// what a symbol is to the sharing
struct FrameSym {
    std::int32_t owner = -1; // the function whose frame holds it; -1: not a frame's
    std::uint32_t size = 0;
    bool param = false; // a parameter: what a call of its function reads
    bool temps = false; // temporaries: read by any call and the return
};

struct FrameFacts {
    std::vector<FrameSym> syms;
    std::vector<bool> candidate;      // by sym: a made local of a frame
    std::vector<bool> pinned;         // by sym: a candidate that keeps bytes of its own
    std::vector<bool> named;          // by sym: some code names it
    std::vector<std::int32_t> called; // by sym: the function a JSR to it calls, else -1
    std::vector<bool> routine;        // by sym: a runtime routine (scratch bytes only)
    // by candidate sym: the syms it meets (one written while the other holds a value)
    std::map<std::int32_t, std::set<std::int32_t>> meets;
    // by candidate sym: the functions called while it holds a value (-1: not known)
    std::map<std::int32_t, std::set<std::int32_t>> calls;
    // by candidate sym: the functions whose code names it
    std::map<std::int32_t, std::set<std::int32_t>> code_of;
};

// one function's code, into facts (syms, candidate, called and routine filled in)
void LookAtCode(FrameFacts& facts, std::int32_t function, const std::vector<AsmItem>& items);

} // namespace tisple
