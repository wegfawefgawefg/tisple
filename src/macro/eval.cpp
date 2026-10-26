#include "macro/eval_internal.hpp"

#include <numbers>

namespace tisple {

namespace {

std::uint32_t FileOf(const MacroRun& run, const Frame& frame, const Node& at) {
    if (at.expansion == 0) {
        return frame.file;
    }
    return run.world->program->sources.expansions[at.expansion - 1].call.file;
}

Loc CallLoc(const MacroRun& run) {
    Loc loc = run.call->call;
    loc.expansion = run.call->expansion;
    return loc;
}

const std::string& HeadOf(const Node& node) {
    static const std::string kEmpty;
    if (node.kind != NodeKind::List || node.items.empty() ||
        node.items[0].kind != NodeKind::Symbol) {
        return kEmpty;
    }
    return node.items[0].text;
}

std::string ParamText(const MacroDef& def) {
    std::string text;
    for (std::size_t i = 0; i < def.params.size(); i++) {
        bool rest = def.rest && i + 1 == def.params.size();
        text += (i == 0 ? "" : " ") + std::string(rest ? "& " : "") + def.params[i];
    }
    return "(" + text + ")";
}

// (table name type count f & options)
std::string CallShape(const MacroDef& def) {
    std::string params = ParamText(def);
    if (def.params.empty()) {
        return "(" + def.name + ")";
    }
    return "(" + def.name + " " + params.substr(1);
}

// fn names written in macro code: the macro, typed fn or library function a name means
enum class Callee : std::uint8_t {
    None,
    Library,
    Macro,
    Typed,
};

Callee FindCallee(const MacroRun& run, std::int32_t module, const std::string& name,
                  const MacroDef*& macro, std::int32_t& fn_module, std::string& fn_name) {
    if (IsLibraryFunction(name)) {
        return Callee::Library;
    }
    macro = FindMacroByName(*run.world, module, name);
    if (macro) {
        return Callee::Macro;
    }
    if (FindTypedFunction(*run.world, module, name, fn_module, fn_name)) {
        return Callee::Typed;
    }
    return Callee::None;
}

Value CallMacroDef(MacroRun& run, const MacroDef& def, std::vector<Value> args, const Node& at,
                   Frame& caller) {
    std::size_t fixed = def.rest ? def.params.size() - 1 : def.params.size();
    bool arity_ok = def.rest ? args.size() >= fixed : args.size() == fixed;
    if (!arity_ok) {
        std::string count = std::to_string(fixed) + " argument" + (fixed == 1 ? "" : "s");
        RunFail(run, caller, at,
                def.name + " takes " + (def.rest ? "at least " : "") + count + ", got " +
                    std::to_string(args.size()) + "; write " + CallShape(def));
        return Value();
    }
    if (run.depth >= kMaxMacroDepth) {
        RunFailHere(run, "macro " + def.name + " nested more than " +
                             std::to_string(kMaxMacroDepth) +
                             " calls deep; does its recursion reach a base case?");
        return Value();
    }
    Frame frame;
    frame.name = "macro " + def.name;
    frame.params = ParamText(def);
    frame.module = def.module;
    frame.file = def.loc.file;
    frame.scopes.emplace_back();
    for (std::size_t i = 0; i < fixed; i++) {
        frame.scopes.back().push_back(Binding{def.params[i], std::move(args[i])});
    }
    if (def.rest) {
        std::vector<Value> more(std::make_move_iterator(args.begin() + static_cast<long>(fixed)),
                                std::make_move_iterator(args.end()));
        frame.scopes.back().push_back(
            Binding{def.params.back(), ListValue(std::move(more), def.module)});
    }
    run.depth++;
    Value value;
    for (const Node& form : def.body) {
        value = Eval(run, frame, form);
        if (run.flow != RunFlow::Normal) {
            break;
        }
    }
    run.depth--;
    if (run.flow == RunFlow::Return) {
        run.flow = RunFlow::Normal;
        value = std::move(run.returned);
    }
    if (run.flow == RunFlow::Break || run.flow == RunFlow::Continue) {
        run.flow = RunFlow::Normal;
        RunFail(run, frame, at, "break and continue only work inside while and for");
    }
    return value;
}

Value CallFn(MacroRun& run, const FnValue& fn, std::vector<Value> args, const Node& at,
             Frame& caller) {
    if (args.size() != fn.params.size()) {
        RunFail(run, caller, at,
                "this fn takes " + std::to_string(fn.params.size()) + " argument" +
                    (fn.params.size() == 1 ? "" : "s") + ", got " + std::to_string(args.size()));
        return Value();
    }
    if (run.depth >= kMaxMacroDepth) {
        RunFailHere(run, "macro code nested more than " + std::to_string(kMaxMacroDepth) +
                             " calls deep; does its recursion reach a base case?");
        return Value();
    }
    Frame frame;
    frame.name = "a fn";
    frame.module = fn.module;
    frame.file = fn.file;
    frame.scopes.push_back(fn.captured);
    frame.scopes.emplace_back();
    frame.params = "(";
    for (std::size_t i = 0; i < args.size(); i++) {
        frame.params += (i == 0 ? "" : " ") + fn.params[i];
        frame.scopes.back().push_back(Binding{fn.params[i], std::move(args[i])});
    }
    frame.params += ")";
    run.depth++;
    Value value;
    for (const Node& form : fn.body) {
        value = Eval(run, frame, form);
        if (run.flow != RunFlow::Normal) {
            break;
        }
    }
    run.depth--;
    if (run.flow == RunFlow::Return) {
        run.flow = RunFlow::Normal;
        value = std::move(run.returned);
    }
    if (run.flow == RunFlow::Break || run.flow == RunFlow::Continue) {
        run.flow = RunFlow::Normal;
        RunFail(run, frame, at, "break and continue only work inside while and for");
    }
    return value;
}

// a (fn (a b) body ...) form handed over as an argument: its names resolve where it is written
bool FnFromForm(const MacroRun& run, const Value& form, FnValue& fn) {
    if (form.kind != ValueKind::List || form.mark != QuoteMark::None || form.items.size() < 3 ||
        form.items[0].kind != ValueKind::Symbol || form.items[0].text != "fn" ||
        form.items[1].kind != ValueKind::List) {
        return false;
    }
    for (const Value& param : form.items[1].items) {
        if (param.kind != ValueKind::Symbol) {
            return false;
        }
        fn.params.push_back(param.text);
    }
    Stamp stamp;
    stamp.call = run.call->call;
    stamp.expansion = run.call->expansion;
    for (std::size_t i = 2; i < form.items.size(); i++) {
        Node node;
        std::string why;
        if (!ValueToNode(form.items[i], stamp, node, why)) {
            return false;
        }
        fn.body.push_back(std::move(node));
    }
    fn.module = form.module;
    fn.file = form.written ? form.file : run.call->call.file;
    return true;
}

Value EvalSymbol(MacroRun& run, Frame& frame, const Node& node) {
    const std::string& name = node.text;
    if (name == "true" || name == "false") {
        return BoolValue(name == "true", frame.module);
    }
    if (name == "pi") {
        return FloatValue(std::numbers::pi, frame.module);
    }
    if (const Binding* binding = FindBinding(frame, name)) {
        return binding->value;
    }
    std::int32_t module = ResolveModule(*run.world, node, frame.module);
    std::int32_t const_module = -1;
    std::string const_name;
    if (FindTypedConst(*run.world, module, name, const_module, const_name)) {
        return ReadTypedConst(run, frame, const_module, const_name, node);
    }
    const MacroDef* macro = nullptr;
    std::int32_t fn_module = -1;
    std::string fn_name;
    if (FindCallee(run, module, name, macro, fn_module, fn_name) != Callee::None) {
        return SymbolValue(name, module); // a function value is its name
    }
    RunFail(run, frame, node,
            name + " is not a parameter " + frame.params + ", let, constant or function; write '" +
                name + " for the symbol");
    return Value();
}

Value EvalCall(MacroRun& run, Frame& frame, const Node& node) {
    const Node& head = node.items[0];
    if (head.kind != NodeKind::Symbol) {
        RunFail(run, frame, node, "macro code calls a function by name: (f a b)");
        return Value();
    }
    std::vector<Value> args;
    args.reserve(node.items.size() - 1);
    for (std::size_t i = 1; i < node.items.size(); i++) {
        args.push_back(Eval(run, frame, node.items[i]));
        if (Failed(run)) {
            return Value();
        }
    }
    if (const Binding* binding = FindBinding(frame, head.text)) {
        Value fn = binding->value;
        return CallValue(run, frame, fn, std::move(args), node);
    }
    std::int32_t module = ResolveModule(*run.world, head, frame.module);
    const MacroDef* macro = nullptr;
    std::int32_t fn_module = -1;
    std::string fn_name;
    switch (FindCallee(run, module, head.text, macro, fn_module, fn_name)) {
    case Callee::Library:
        return CallLibrary(run, frame, head.text, args, node);
    case Callee::Macro:
        return CallMacroDef(run, *macro, std::move(args), node, frame);
    case Callee::Typed:
        return CallTyped(run, frame, fn_module, fn_name, args, node);
    case Callee::None:
        break;
    }
    RunFail(run, frame, head,
            "unknown function " + head.text + " in " + frame.name +
                "; macro code calls macros, fns and list, map, range, ...");
    return Value();
}

} // namespace

bool Failed(const MacroRun& run) {
    return run.flow == RunFlow::Failed;
}

void RunFail(MacroRun& run, const Frame& frame, const Node& at, const std::string& message) {
    if (Failed(run)) {
        return;
    }
    Loc code = LocOf(FileOf(run, frame, at), at);
    code.expansion = 0;
    std::string where = FormatLoc(run.world->program->sources, code);
    AddError(*run.world->errors, CallLoc(run), message + "; macro code at " + where);
    run.flow = RunFlow::Failed;
}

void RunFailHere(MacroRun& run, const std::string& message) {
    if (Failed(run)) {
        return;
    }
    AddError(*run.world->errors, CallLoc(run), message);
    run.flow = RunFlow::Failed;
}

bool Step(MacroRun& run, std::uint64_t count) {
    run.steps += count;
    if (run.steps <= kMaxMacroSteps) {
        return true;
    }
    RunFailHere(run, "macro " + run.call->def->name + " ran more than " +
                         std::to_string(kMaxMacroSteps) + " steps; is a loop missing its end?");
    return false;
}

Binding* FindBinding(Frame& frame, const std::string& name) {
    for (auto scope = frame.scopes.rbegin(); scope != frame.scopes.rend(); ++scope) {
        for (auto entry = scope->rbegin(); entry != scope->rend(); ++entry) {
            if (entry->name == name) {
                return &*entry;
            }
        }
    }
    return nullptr;
}

bool IsCallable(const MacroRun& run, const Value& fn) {
    if (fn.kind == ValueKind::Fn) {
        return true;
    }
    if (fn.kind == ValueKind::Symbol) {
        const MacroDef* macro = nullptr;
        std::int32_t fn_module = -1;
        std::string fn_name;
        return FindCallee(run, fn.module, fn.text, macro, fn_module, fn_name) != Callee::None;
    }
    FnValue form;
    return FnFromForm(run, fn, form);
}

Value CallValue(MacroRun& run, Frame& frame, const Value& fn, std::vector<Value> args,
                const Node& at) {
    if (fn.kind == ValueKind::Fn) {
        return CallFn(run, *fn.fn, std::move(args), at, frame);
    }
    if (fn.kind == ValueKind::Symbol) {
        const MacroDef* macro = nullptr;
        std::int32_t fn_module = -1;
        std::string fn_name;
        switch (FindCallee(run, fn.module, fn.text, macro, fn_module, fn_name)) {
        case Callee::Library:
            return CallLibrary(run, frame, fn.text, args, at);
        case Callee::Macro:
            return CallMacroDef(run, *macro, std::move(args), at, frame);
        case Callee::Typed:
            return CallTyped(run, frame, fn_module, fn_name, args, at);
        case Callee::None:
            break;
        }
        RunFail(run, frame, at,
                fn.text + " names no function here; pass a macro, a fn or a function's name");
        return Value();
    }
    FnValue form;
    if (FnFromForm(run, fn, form)) {
        return CallFn(run, form, std::move(args), at, frame);
    }
    RunFail(run, frame, at,
            DescribeValue(fn) + " is not a function; pass a macro, a fn or a function's name");
    return Value();
}

Value Eval(MacroRun& run, Frame& frame, const Node& node) {
    if (Failed(run) || !Step(run, 1)) {
        return Value();
    }
    switch (node.kind) {
    case NodeKind::Integer:
    case NodeKind::String:
    case NodeKind::Keyword:
        return NodeToValue(node, false, frame.file, frame.module);
    case NodeKind::Symbol:
        return EvalSymbol(run, frame, node);
    case NodeKind::List:
        break;
    }
    if (node.items.empty()) {
        RunFail(run, frame, node, "() is not macro code; write (list) for an empty list");
        return Value();
    }
    Value out;
    if (EvalSpecialForm(run, frame, node, HeadOf(node), out)) {
        return out;
    }
    return EvalCall(run, frame, node);
}

std::int32_t ResolveModule(const MacroWorld& world, const Node& node, std::int32_t module) {
    if (node.expansion == 0) {
        return module;
    }
    std::int32_t own = world.program->sources.expansions[node.expansion - 1].module;
    return own >= 0 ? own : module;
}

bool SplitName(const MacroWorld& world, std::int32_t module, const std::string& name,
               std::int32_t& target, std::string& plain) {
    if (module < 0) {
        return false;
    }
    std::size_t dot = name.find('.');
    if (dot == std::string::npos) {
        target = module;
        plain = name;
        return true;
    }
    std::string prefix = name.substr(0, dot);
    plain = name.substr(dot + 1);
    for (const Import& import : world.program->modules[static_cast<std::size_t>(module)].imports) {
        if (import.prefix == prefix) {
            target = import.module;
            return target >= 0;
        }
    }
    return false;
}

const MacroDef* FindMacroByName(const MacroWorld& world, std::int32_t module,
                                const std::string& name) {
    std::int32_t target = -1;
    std::string plain;
    if (!SplitName(world, module, name, target, plain)) {
        return nullptr;
    }
    const auto& names = world.macros_by_module[static_cast<std::size_t>(target)];
    auto it = names.find(plain);
    return it == names.end() ? nullptr : &world.macros[static_cast<std::size_t>(it->second)];
}

bool RunMacro(MacroWorld& world, const MacroCall& call, Node& out) {
    MacroRun run;
    run.world = &world;
    run.call = &call;
    Frame caller;
    caller.name = "macro " + call.def->name;
    caller.module = call.module;
    caller.file = call.call.file;
    Node at;
    at.span = call.call.span;
    Value value = CallMacroDef(run, *call.def, call.args, at, caller);
    if (Failed(run)) {
        return false;
    }
    Stamp stamp;
    stamp.macro = call.def->name;
    stamp.call = call.call;
    stamp.expansion = call.expansion;
    stamp.module = call.def->module;
    stamp.sources = &world.program->sources;
    std::string why;
    if (!ValueToNode(value, stamp, out, why)) {
        RunFailHere(run, "macro " + call.def->name + ": " + why);
        return false;
    }
    return true;
}

} // namespace tisple
