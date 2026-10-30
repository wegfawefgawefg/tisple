#pragma once

// Constant propagation: consts_walk.cpp works out the values (consts_eval.cpp each node's
// from its operands'), consts.cpp rewrites the tree.

#include "opt/opt_internal.hpp"

namespace tisple::opt {

// what a variable or expression may hold at a point
struct Val {
    enum Kind : std::uint8_t {
        None,  // no run reaches here yet
        Const, // always these bits (masked to the type)
        Any,   // more than one value, or one the analysis cannot see
    } kind = None;
    std::uint16_t bits = 0;

    bool operator==(const Val& o) const {
        return kind == o.kind && bits == o.bits;
    }
};

// by expr: the join of every value it had on every run the walk followed (None where no run
// went); a node's value is only ever read where it is an rvalue
struct ConstFacts {
    std::vector<Val> values;
};

// the values of function's expressions, from a walk of its body that follows only the arms
// of branches whose conditions it cannot decide
void FindConstants(const Pass& p, std::int32_t function, ConstFacts& facts);

// the value the walk found for id, None for nodes it never reached
Val ValueOf(const ConstFacts& facts, ExprId id);

// integers, bools and enums: the scalars the analysis follows (not pointers)
bool ConstType(const Program& program, TypeId type);

// ---- facts from conditions (consts_facts.cpp)

// var holds the value of other where a condition came out as it did
struct Equality {
    std::int32_t var = -1;
    ExprId other = -1; // -1: var is the bool truth
    bool truth = false;
};

// what cond coming out truth says: (= x k) true, (!= x k) false, through not, and and or
void FindEqualities(const Pass& p, ExprId cond, bool truth, std::vector<Equality>& out);

// ---- values from operands' values (consts_eval.cpp)

Val AnyVal();
// these bits, masked to the type
Val ConstOf(const Pass& p, TypeId type, std::uint32_t bits);
Val Join(Val a, Val b);
// worked out twice in a row, it gives the same value: pure, no loads, no :address or aux
// variables
bool Steady(const Pass& p, ExprId id);
Val BinaryValue(const Pass& p, const Expr& e, Val a, Val b);
Val UnaryValue(const Pass& p, const Expr& e, Val a);
Val ConvertValue(const Pass& p, const Expr& e, Val a);
// where a place lies in const data, as far as its indexes are known
struct ConstAt {
    std::int32_t var = -1; // -1: not in const data, or an index not known
    std::uint32_t offset = 0;
};
// a var's own place: in const data when it is a const
ConstAt ConstRoot(const Pass& p, std::int32_t var);
// (at base k) for a known k inside the array
ConstAt ElementAt(const Pass& p, const Expr& index, ConstAt base, Val k);
// a read of const data at a known place: its value
Val ConstReadValue(const Pass& p, const Expr& read, ConstAt at);
// a load of a scalar at a constant address into const data ((load (+ "HELLO" 1)))
Val LoadValue(const Pass& p, ExprId load);
// a pointer or function pointer read of const data at a constant place where the link fills in
// an address becomes that address (const_data.cpp); true when id was rewritten
bool FoldAddressRead(Pass& p, ExprId id);

} // namespace tisple::opt
