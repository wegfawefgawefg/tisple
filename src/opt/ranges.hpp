#pragma once

// Value ranges: ranges_values.cpp works out what an expression may hold, ranges.cpp decides
// compares and match clauses with it, ranges_vars.cpp works out once a round what each
// variable may hold anywhere and which bytes of arrays stores may write.

#include "opt/opt_internal.hpp"

namespace tisple::opt {

// var lies in range here: a loop's counter in its body, or (fact) what the last set of var
// in a do before this point gave it
struct Counter {
    std::int32_t var = -1; // -1: a fact a later write undid
    Range range;
    bool fact = false;
};

using Counters = std::vector<Counter>; // the loops and sets around a node, innermost last

// every value of the type
Range TypeRange(const Pass& p, TypeId type);
// what id may hold, inside the loops whose counters are given
Range ValueRange(const Pass& p, const Counters& counters, ExprId id);
// pure, and reads no hardware (:address, aux, loads), which may answer differently or change
// when read: safe to work out never
bool Quiet(const Pass& p, ExprId id);
// the counter of a for, or of a counting while ((let c v) as before, the item just before
// it), and the values it takes in the body
bool LoopCounter(const Pass& p, const Counters& counters, ExprId loop, ExprId before, Counter& out);
// r in the other signedness of kind's width: a u16 range of 65535..65535 is -1..-1, one
// that crosses the sign bit is the whole other range
Range OtherView(TypeKind kind, Range r);
// the sets of arrays and structs one walk of the program saw (ranges_elements.cpp)
struct ElementStores {
    std::vector<std::vector<Range>> stored; // by var: packed byte ranges a set may write
    bool in_bounds = true;                  // no set's index may fall outside its array
    std::vector<bool> runs;                 // by function: may it run (the rest write nothing)
};

// a set's place: the bytes of its root it may write, into stores (nothing when it is behind a
// pointer)
void NoteStore(const Pass& p, const Counters& counters, ExprId place, ElementStores& stores);
// the stored bytes of every global covered, merged, into p.ranges
void SettleStores(Pass& p, const ElementStores& stores);
// the index expression of an element read, (at a i) or (at a i x), or -1 for a field of a
// struct variable
ExprId ReadIndex(const Pass& p, const Expr& read);
// the values a read of an element or field may give when no store reaches what it reads,
// its index lying in index (ranges_vars.cpp); false when a store may, or when the round's
// stored bytes do not cover its variable
bool UnstoredRead(const Pass& p, const Expr& read, Range index, Range& out);
// a variable the round's ranges follow: an integer, bool or enum with no address taken, not
// :address, aux or :reserve
bool RangedVar(const Pass& p, std::int32_t var);

} // namespace tisple::opt
