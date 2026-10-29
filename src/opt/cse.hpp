#pragma once

// Common subexpressions: cse_walk.cpp finds where a value is worked out again while its first
// working out still holds, cse.cpp keeps the first in a new local and reads it there.

#include "opt/opt_internal.hpp"

namespace tisple::opt {

// a place a let may go: a do (before one of its items), or an arm of a void if or match that
// is not a do (wrapped in one first)
struct CseBlock {
    ExprId id = -1;
    bool slot = false;
    ExprId parent = -1;        // slot: the if or match
    std::uint32_t which = 0;   // slot: 0 then, 1 else, 2 + i clause i
    std::uint32_t stmt = 0;    // the item being walked
    std::size_t log_start = 0; // the kill log's length when that item began
};

// where an occurrence sits: its block (-1 for none a let may go in) and item
struct CseAt {
    std::int32_t block = -1;
    std::uint32_t stmt = 0;
};

// a value worked out at first, and where it is worked out again while it still holds
struct CseEntry {
    ExprId first = -1;
    CseAt at;                        // where its let goes: before this item
    bool own = true;                 // a let may go there (else only next to its partner's)
    std::vector<std::int32_t> reads; // vars its value reads, those of pure calls in it too
    bool pointed = false;            // reads an address-taken var a pointer store may change
    std::vector<ExprId> later;       // the same value again
    // a division whose remainder (or the other way round) an earlier entry that still held
    // works out: that entry, whose let this one's may join
    std::int32_t partner = -1;
    // the entries made from this index up to this one's are parts of its value
    std::size_t parts_from = 0;
};

// what a statement (or an effect inside one) may change, logged in walk order
struct CseKill {
    std::int32_t var = -1; // a var set or let; -1 with neither flag: nothing
    bool stores = false;   // through a pointer: address-taken vars
    bool all = false;
};

struct CseFound {
    std::vector<CseEntry> entries;
    std::vector<CseBlock> blocks;
};

// every value in function worth working out once that is worked out again while it holds
void FindCommon(const Pass& p, std::int32_t function, CseFound& out);

// ---- values (cse_values.cpp)

// a value worth keeping in a local when it is worked out again, the same each time it is
// worked out while nothing it reads changes; e.reads gets what it reads
bool CseValue(const Pass& p, ExprId id, CseEntry& e);
// the same value, written the same way (calls included)
bool SameValue(const Pass& p, ExprId a, ExprId b);
// the two divisions (/ a b) and (% a b) of the same operands, of the same type
bool DivisionPartners(const Pass& p, ExprId a, ExprId b);

} // namespace tisple::opt
