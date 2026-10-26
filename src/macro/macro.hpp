#pragma once

// Computing macros: a macro body is code that runs in the compiler and gives the forms to
// paste in (docs/language.md, Macros). This is the evaluator; check/expand.cpp finds the
// calls, hands their argument forms here and pastes the result.
//
// - Macro code uses Tisple's forms (let, set, if, do, while, for, match, return, break,
//   continue, and, or, not, fn, quasiquote) without types, over the values in value.hpp.
// - A macro called from Tisple code gets the argument forms; called from macro code it gets
//   the argument values. Forms are values, so it is one rule.
// - Ordinary Tisple functions run in the reference interpreter with their declared types
//   (typed_call.cpp), so they wrap exactly as on the Apple II.
// - Every evaluated form is a step. A run (one call from Tisple code) stops with an error
//   past kMaxMacroSteps steps or kMaxMacroDepth nested calls.

#include "check/load.hpp"
#include "macro/value.hpp"

#include <functional>
#include <memory>
#include <unordered_map>

namespace tisple {

constexpr std::uint64_t kMaxMacroSteps = 10000000;
constexpr int kMaxMacroDepth = 200;

struct MacroDef {
    std::string name;
    std::int32_t module = -1;
    std::vector<std::string> params;
    bool rest = false; // (a & more): the last parameter takes the other arguments as a list
    std::vector<Node> body;
    Loc loc;
};

struct TypedCalls; // typed_call.cpp

// what macro code sees of the program
struct MacroWorld {
    LoadedProgram* program = nullptr;
    std::vector<Diagnostic>* errors = nullptr;
    std::vector<MacroDef> macros;
    std::vector<std::unordered_map<std::string, std::int32_t>> macros_by_module;
    // top-level fn, const, let, struct and enum forms as written, by module: index into the
    // module's forms. Typed calls build their program from these
    std::vector<std::unordered_map<std::string, std::size_t>> decls_by_module;
    std::uint32_t fresh_counter = 0; // tmp# becomes tmp__n
    // expands one top-level form in place (a copy, for a typed call); false on errors
    std::function<bool(std::int32_t module, Node& form, std::vector<Diagnostic>& errors)>
        expand_form;
    std::shared_ptr<TypedCalls> typed;
};

// one call from Tisple code
struct MacroCall {
    const MacroDef* def = nullptr;
    std::vector<Value> args;
    Loc call;                    // the call form, its expansion id included
    std::int32_t module = -1;    // where the call is written
    std::uint32_t expansion = 0; // the Expansion this call made: what its forms are stamped with
};

// runs the macro and gives the forms to paste in; false after adding an error
bool RunMacro(MacroWorld& world, const MacroCall& call, Node& out);

// the module a name in this form resolves in: the macro's own module for forms a template
// wrote, the given one for forms written in the source
std::int32_t ResolveModule(const MacroWorld& world, const Node& node, std::int32_t module);

const MacroDef* FindMacroByName(const MacroWorld& world, std::int32_t module,
                                const std::string& name);

// names only macro code knows: list, map, range, sin, ... and pi
bool IsMacroLibraryName(const std::string& name);

} // namespace tisple
