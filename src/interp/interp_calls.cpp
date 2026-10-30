// Calls in the interpreter: direct, through a function pointer, and the interrupt handler,
// which runs before a step when an interrupt is due and interrupts are on, as an IRQ would
// between two instructions.

#include "interp/interp_internal.hpp"
#include "link/image.hpp"

#include <vector>

namespace tisple::interp {

std::uint16_t CallInto(Interp& in, std::int32_t function, const std::uint16_t* args,
                       std::uint32_t count) {
    const Function& fn = in.program->functions[static_cast<std::size_t>(function)];
    for (std::uint32_t i = 0; i < count; i++) {
        std::int32_t param = fn.params[i];
        TypeId type = in.program->vars[static_cast<std::size_t>(param)].type;
        WriteScalar(in, VarCursor(*in.layout, param), TypeAt(in, type).size, args[i]);
    }
    std::uint16_t value = Eval(in, fn.body);
    if (in.flow == Flow::Return) {
        in.flow = Flow::Normal;
        value = in.return_value;
    }
    return fn.ret == kVoidType ? 0 : value;
}

std::uint16_t EvalCall(Interp& in, const Expr& e) {
    std::uint16_t args[64];
    std::vector<std::uint16_t> many;
    std::uint16_t* slots = args;
    if (e.count > 64) {
        many.resize(e.count);
        slots = many.data();
    }
    std::uint16_t pointer = 0;
    if (e.kind == ExprKind::CallPtr) {
        pointer = Eval(in, e.a);
        if (in.flow == Flow::Stop) {
            return 0;
        }
    }
    for (std::uint32_t i = 0; i < e.count; i++) {
        slots[i] = Eval(in, in.program->lists[e.first + i]);
        if (in.flow == Flow::Stop) {
            return 0;
        }
    }
    if (e.kind == ExprKind::Call) {
        return CallInto(in, e.ref, slots, e.count);
    }
    // through a pointer: the function whose value it holds, of the pointer's own type
    std::int32_t function = FunctionOfValue(*in.program, *in.layout, pointer);
    TypeId type = At(in, e.a).type;
    if (function < 0 || in.program->functions[static_cast<std::size_t>(function)].type != type) {
        Stop(in, StopKind::Error,
             "a call through a function pointer that holds no function of its type", e.loc);
        return 0;
    }
    return CallInto(in, function, slots, e.count);
}

void TakeInterrupt(Interp& in) {
    std::int32_t handler = in.program->handler;
    if (!in.irq_on || in.in_handler || handler < 0) {
        return; // pending until (cli), or after the handler's return
    }
    in.irq_next++;
    in.irqs_taken++;
    // what the code around was doing stays as it is: only memory changes
    Flow flow = in.flow;
    std::uint16_t return_value = in.return_value;
    in.in_handler = true;
    in.irq_on = false; // the cpu sets I; RTI restores it
    CallInto(in, handler, nullptr, 0);
    in.in_handler = false;
    in.irq_on = true;
    if (in.flow != Flow::Stop) {
        in.flow = flow;
        in.return_value = return_value;
    }
}

} // namespace tisple::interp

namespace tisple {

InterpResult CallFunction(Interp& in, std::int32_t function,
                          const std::vector<std::uint16_t>& args) {
    in.steps = 0;
    in.flow = Flow::Normal;
    in.stop = StopKind::Finished;
    in.message.clear();
    InterpResult result;
    const Function& fn = in.program->functions[static_cast<std::size_t>(function)];
    if (args.size() != fn.params.size()) {
        result.kind = StopKind::Error;
        result.message = fn.name + " takes " + std::to_string(fn.params.size()) + " arguments";
        result.loc = fn.loc;
        return result;
    }
    std::uint16_t value =
        interp::CallInto(in, function, args.data(), static_cast<std::uint32_t>(args.size()));
    result.steps = in.steps;
    if (in.flow == Flow::Stop) {
        result.kind = in.stop;
        result.message = in.message;
        result.loc = in.stop_loc;
        return result;
    }
    result.kind = StopKind::Finished;
    result.type = fn.ret;
    result.has_value = fn.ret != kVoidType;
    result.value = value;
    return result;
}

} // namespace tisple
