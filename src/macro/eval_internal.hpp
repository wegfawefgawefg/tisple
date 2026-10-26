#pragma once

// shared by eval.cpp (forms), library.cpp (list, map, sin, ...) and typed_call.cpp

#include "macro/macro.hpp"

namespace tisple {

enum class RunFlow : std::uint8_t {
    Normal,
    Break,
    Continue,
    Return,
    Failed,
};

// one call from Tisple code, with everything it calls at compile time
struct MacroRun {
    MacroWorld* world = nullptr;
    const MacroCall* call = nullptr;
    std::uint64_t steps = 0;
    int depth = 0;
    RunFlow flow = RunFlow::Normal;
    Value returned;
};

// one body running: a macro's or a fn's
struct Frame {
    std::string name;         // "macro table", "a fn"
    std::string params;       // "(name type count f)", for messages
    std::int32_t module = -1; // where names resolve
    std::uint32_t file = 0;   // where the code is written
    std::vector<std::vector<Binding>> scopes;
    std::unordered_map<std::string, std::string> fresh; // tmp# -> tmp__n, per call
};

bool Failed(const MacroRun& run);
// an error at the Tisple call; at is the macro code it came from
void RunFail(MacroRun& run, const Frame& frame, const Node& at, const std::string& message);
// an error at the Tisple call alone: limits and (error ...)
void RunFailHere(MacroRun& run, const std::string& message);
// counts one step; false (after the error) past the limit
bool Step(MacroRun& run, std::uint64_t count);

Value Eval(MacroRun& run, Frame& frame, const Node& node);
// fn is a fn, a function's name, or a (fn (a) ...) form from an argument
Value CallValue(MacroRun& run, Frame& frame, const Value& fn, std::vector<Value> args,
                const Node& at);
bool IsCallable(const MacroRun& run, const Value& fn);
Binding* FindBinding(Frame& frame, const std::string& name);

// forms.cpp: let, if, while, quasiquote, ...; false when head is not one of them
bool EvalSpecialForm(MacroRun& run, Frame& frame, const Node& node, const std::string& head,
                     Value& out);
// the body forms node.items[first..] in a scope of their own; the last one's value
Value EvalBody(MacroRun& run, Frame& frame, const Node& node, std::size_t first);
// a parameter, let or for name: plain, and not a name macro code already gives a meaning
bool CheckBindingName(MacroRun& run, const Frame& frame, const Node& name, const char* what);

// library.cpp: operators and the functions among IsMacroLibraryName
bool IsLibraryFunction(const std::string& name);
Value CallLibrary(MacroRun& run, Frame& frame, const std::string& name, std::vector<Value>& args,
                  const Node& at);

// typed_call.cpp: an ordinary Tisple fn or constant, by its (maybe prefixed) name
bool FindTypedFunction(const MacroWorld& world, std::int32_t module, const std::string& name,
                       std::int32_t& fn_module, std::string& fn_name);
Value CallTyped(MacroRun& run, Frame& frame, std::int32_t fn_module, const std::string& fn_name,
                const std::vector<Value>& args, const Node& at);

bool FindTypedConst(const MacroWorld& world, std::int32_t module, const std::string& name,
                    std::int32_t& const_module, std::string& const_name);
Value ReadTypedConst(MacroRun& run, Frame& frame, std::int32_t const_module,
                     const std::string& const_name, const Node& at);

// "lores.row_start" through an import of module, or a plain name in module itself
bool SplitName(const MacroWorld& world, std::int32_t module, const std::string& name,
               std::int32_t& target, std::string& plain);

} // namespace tisple
