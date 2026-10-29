#pragma once

// Value ranges: ranges_values.cpp works out what an expression may hold, ranges.cpp decides
// compares and match clauses with it.

#include "opt/opt_internal.hpp"

namespace tisple::opt {

// mathematical values, lo to hi
struct Range {
    std::int64_t lo = 0;
    std::int64_t hi = 0;
};

struct Counter {
    std::int32_t var = -1;
    Range range;
};

using Counters = std::vector<Counter>; // the loops around a node, innermost last

// every value of the type
Range TypeRange(const Pass& p, TypeId type);
// what id may hold, inside the loops whose counters are given
Range ValueRange(const Pass& p, const Counters& counters, ExprId id);
// pure, and reads no hardware (:address, aux, loads), which may answer differently or change
// when read: safe to work out never
bool Quiet(const Pass& p, ExprId id);

} // namespace tisple::opt
