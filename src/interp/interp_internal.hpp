#pragma once

// Shared by the interpreter's files: interp.cpp evaluates, interp_calls.cpp calls functions,
// directly, through pointers and as the interrupt handler.

#include "interp/interp.hpp"

#include <string>

namespace tisple::interp {

const Expr& At(const Interp& in, ExprId id);
const Type& TypeAt(const Interp& in, TypeId id);
void Stop(Interp& in, StopKind kind, const std::string& message, const Loc& loc);
// one step: the expression's value
std::uint16_t Eval(Interp& in, ExprId id);

// the function with its arguments in its parameters; its value
std::uint16_t CallInto(Interp& in, std::int32_t function, const std::uint16_t* args,
                       std::uint32_t count);
// a Call or a CallPtr
std::uint16_t EvalCall(Interp& in, const Expr& e);
// an interrupt is due (irq_steps): the handler runs now if interrupts are on
void TakeInterrupt(Interp& in);

} // namespace tisple::interp
