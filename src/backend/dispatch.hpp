#pragma once

// Calls through function pointers, made into direct calls once the mid-level passes are done
// (CompileProgram), so every analysis of the backend and its code generator see ordinary
// calls and static frames:
//
// - Each (fn ...) type t gets a dispatcher, a function whose parameters are the pointer and
//   then t's parameters, and whose code stores the pointer into its own JMP and jumps there
//   (programs run from RAM). (p a b) becomes (dispatcher p a b).
// - Each function f whose address is taken gets an entry, f.indirect, with no parameters: it
//   calls f with the dispatcher's parameters and gives what f gives. A function pointer's
//   value is the address of that entry.
// - The dispatcher's callees are the entries of t's functions, and each entry's callee is its
//   function, so frames stack caller, dispatcher, entry, function: the entry reads the
//   arguments in the dispatcher's frame, which nothing else overlaps while it runs. f's
//   parameters get their values the way any call gives them, so value ranges and pointer
//   targets follow them as they follow every call.
// - The dispatcher's body is never generated; for the analyses it is a match that calls one
//   of the entries (its effects, its return value's range and targets).

#include "backend/gen.hpp"

namespace tisple {

// rewrites program; by function id of the program as it was: its entry function, or -1 when
// its address is never taken
std::vector<std::int32_t> LowerIndirectCalls(Program& program);

// a dispatcher's code (Function::dispatcher)
void GenDispatcher(Backend& be, std::int32_t function);

} // namespace tisple
