// Ordinary Tisple functions called from macro code, and scalar constants read by it. The
// functions run in the reference interpreter with their declared types, so (row_start 200)
// wraps exactly as it would on the Apple II.
//
// They are checked as a small program of their own: the functions and constants macro code
// uses, plus every top-level declaration their names reach, copied and expanded. It is built the
// first time a macro calls a typed function and again whenever another one is called. Declarations
// a top-level macro call makes are not part of it.
//
// A function may not touch the hardware, write through a pointer, or read or write a top-level
// let: at compile time those would see a machine that is not running.

#include "check/arith.hpp"
#include "check/check.hpp"
#include "check/name_index.hpp"
#include "interp/interp.hpp"
#include "link/layout.hpp"
#include "macro/eval_internal.hpp"

namespace tisple {

struct TypedCalls {
    std::vector<std::pair<std::int32_t, std::string>> roots; // module, fn or const name
    bool built = false;
    bool ok = false;
    bool building = false;
    std::string failure; // the first error of a failed build
    Program program;
    Layout layout;
    Interp interp;
    std::vector<ConstInfo> consts;
    std::vector<std::string> impure; // by function id: why it can't run here; "" when it can
    std::vector<bool> scanned;
};

namespace {

const std::string& FormHead(const Node& node) {
    static const std::string kEmpty;
    if (node.kind != NodeKind::List || node.items.empty() ||
        node.items[0].kind != NodeKind::Symbol) {
        return kEmpty;
    }
    return node.items[0].text;
}

// ---- which declarations the called functions reach

struct Reach {
    const MacroWorld* world = nullptr;
    std::vector<std::vector<bool>> included;
    std::vector<std::pair<std::int32_t, std::size_t>> work;
};

void Want(Reach& reach, std::int32_t module, const std::string& name) {
    const auto& decls = reach.world->decls_by_module[static_cast<std::size_t>(module)];
    auto it = decls.find(name);
    if (it != decls.end()) {
        reach.work.push_back({module, it->second});
    }
}

void ScanNames(Reach& reach, std::int32_t module, const Node& node) {
    if (node.kind == NodeKind::List) {
        for (const Node& item : node.items) {
            ScanNames(reach, module, item);
        }
        return;
    }
    if (node.kind != NodeKind::Symbol) {
        return;
    }
    std::int32_t at = ResolveModule(*reach.world, node, module);
    const std::string& text = node.text;
    std::size_t dot = text.find('.');
    Want(reach, at, text.substr(0, dot));
    if (dot == std::string::npos) {
        return;
    }
    std::string prefix = text.substr(0, dot);
    std::size_t end = text.find('.', dot + 1);
    std::string member = text.substr(dot + 1, end == std::string::npos ? end : end - dot - 1);
    for (const Import& import :
         reach.world->program->modules[static_cast<std::size_t>(at)].imports) {
        if (import.prefix == prefix && import.module >= 0) {
            Want(reach, import.module, member);
        }
    }
}

// the program the roots need, expanded and checked; false with typed.failure
bool BuildTyped(MacroWorld& world, TypedCalls& typed) {
    const LoadedProgram& loaded = *world.program;
    Reach reach;
    reach.world = &world;
    reach.included.resize(loaded.modules.size());
    for (std::size_t m = 0; m < loaded.modules.size(); m++) {
        reach.included[m].assign(loaded.modules[m].tree.forms.size(), false);
    }
    for (const auto& root : typed.roots) {
        Want(reach, root.first, root.second);
    }
    while (!reach.work.empty()) {
        auto [module, index] = reach.work.back();
        reach.work.pop_back();
        std::vector<bool>& flags = reach.included[static_cast<std::size_t>(module)];
        if (flags[index]) {
            continue;
        }
        flags[index] = true;
        ScanNames(reach, module,
                  loaded.modules[static_cast<std::size_t>(module)].tree.forms[index]);
    }

    LoadedProgram sub;
    std::vector<Diagnostic> errors;
    for (std::size_t m = 0; m < loaded.modules.size(); m++) {
        const Module& source = loaded.modules[m];
        Module module;
        module.name = source.name;
        module.file = source.file;
        module.imports = source.imports;
        for (std::size_t i = 0; i < source.tree.forms.size(); i++) {
            const Node& form = source.tree.forms[i];
            if (!reach.included[m][i] && FormHead(form) != "import") {
                continue;
            }
            module.tree.forms.push_back(form);
            if (!world.expand_form(static_cast<std::int32_t>(m), module.tree.forms.back(),
                                   errors)) {
                break;
            }
        }
        sub.modules.push_back(std::move(module));
    }
    sub.sources = loaded.sources;
    NameIndex names;
    if (errors.empty()) {
        CheckProgram(sub, typed.program, errors, &names);
        typed.consts = std::move(names.consts);
    } else {
        typed.program.sources = std::move(sub.sources);
    }
    if (errors.empty()) {
        LayoutProgram(typed.program, LayoutOptions(), typed.layout, errors);
    }
    if (!errors.empty()) {
        typed.failure = FormatDiagnostic(typed.program.sources, errors[0]);
        return false;
    }
    InitInterp(typed.interp, typed.program, typed.layout);
    typed.impure.assign(typed.program.functions.size(), "");
    typed.scanned.assign(typed.program.functions.size(), false);
    return true;
}

// ---- what a function does that it can't do at compile time

std::string ExprWhy(const Program& program, ExprId id);

std::string ListWhy(const Program& program, std::uint32_t first, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; i++) {
        std::string why = ExprWhy(program, program.lists[first + i]);
        if (!why.empty()) {
            return why;
        }
    }
    return "";
}

std::string ExprWhy(const Program& program, ExprId id) {
    if (id < 0) {
        return "";
    }
    const Expr& e = program.exprs[static_cast<std::size_t>(id)];
    switch (e.kind) {
    case ExprKind::IoRead:
        return "uses io_read";
    case ExprKind::IoWrite:
        return "uses io_write";
    case ExprKind::Store:
    case ExprKind::StoreField:
        return "writes through a pointer";
    case ExprKind::VarRef: {
        const Var& var = program.vars[static_cast<std::size_t>(e.ref)];
        if (var.kind == VarKind::Global) {
            return "uses the top-level let " + var.name + ", which only has a value at run time";
        }
        return "";
    }
    case ExprKind::Call:
    case ExprKind::StructLit:
    case ExprKind::Do:
        return ListWhy(program, e.first, e.count);
    case ExprKind::Match: {
        std::string why = ExprWhy(program, e.a);
        for (std::uint32_t i = 0; i < e.count && why.empty(); i++) {
            why = ExprWhy(program, program.clauses[e.first + i].body);
        }
        return why;
    }
    default:
        break;
    }
    std::string why = ExprWhy(program, e.a);
    if (why.empty()) {
        why = ExprWhy(program, e.b);
    }
    if (why.empty()) {
        why = ExprWhy(program, e.c);
    }
    return why;
}

// why the function, or one it calls, can't run at compile time; "" when it can
const std::string& FunctionWhy(TypedCalls& typed, std::int32_t function) {
    std::size_t index = static_cast<std::size_t>(function);
    if (typed.scanned[index]) {
        return typed.impure[index];
    }
    typed.scanned[index] = true; // no recursion: the checker rejects call cycles
    const Function& fn = typed.program.functions[index];
    std::string why = ExprWhy(typed.program, fn.body);
    for (std::size_t i = 0; i < fn.callees.size() && why.empty(); i++) {
        const std::string& inner = FunctionWhy(typed, fn.callees[i]);
        if (!inner.empty()) {
            why = "calls " + typed.program.functions[static_cast<std::size_t>(fn.callees[i])].name +
                  ", which " + inner;
        }
    }
    typed.impure[index] = why;
    return typed.impure[index];
}

std::string Signature(const Program& program, const Function& fn) {
    std::string text;
    for (std::int32_t param : fn.params) {
        const Var& var = program.vars[static_cast<std::size_t>(param)];
        text += (text.empty() ? "(" : " (") + var.name + " " + TypeName(program, var.type) + ")";
    }
    return "(" + text + ")";
}

// a macro value as the raw bits of a typed argument
bool ArgBits(const Program& program, TypeId type, const Value& value, std::uint16_t& bits,
             std::string& why) {
    TypeKind kind = KindOf(program, type);
    std::string name = TypeName(program, type);
    if (kind == TypeKind::Bool) {
        if (value.kind != ValueKind::Bool) {
            why = name + " takes true or false, got " + DescribeValue(value);
            return false;
        }
        bits = static_cast<std::uint16_t>(value.integer);
        return true;
    }
    std::int64_t low = 0;
    std::int64_t high = 0;
    if (kind == TypeKind::U8) {
        high = 255;
    } else if (kind == TypeKind::I8) {
        low = -128;
        high = 127;
    } else if (kind == TypeKind::U16 || kind == TypeKind::Ptr) {
        high = 65535;
    } else if (kind == TypeKind::I16) {
        low = -32768;
        high = 32767;
    } else {
        why = "compile-time calls pass integers and bools, not " + name;
        return false;
    }
    if (value.kind != ValueKind::Int) {
        why = name + " takes an integer, got " + DescribeValue(value);
        return false;
    }
    if (value.integer < low || value.integer > high) {
        why = ShowValue(value) + " does not fit in " + name + " (" + std::to_string(low) + " to " +
              std::to_string(high) + ")";
        return false;
    }
    bits = static_cast<std::uint16_t>(value.integer & 0xFFFF);
    return true;
}

bool HasRoot(const TypedCalls& typed, std::int32_t module, const std::string& name) {
    for (const auto& root : typed.roots) {
        if (root.first == module && root.second == name) {
            return true;
        }
    }
    return false;
}

bool FindDecl(const MacroWorld& world, std::int32_t module, const std::string& name,
              const char* head, std::int32_t& decl_module, std::string& decl_name) {
    if (!SplitName(world, module, name, decl_module, decl_name)) {
        return false;
    }
    const auto& decls = world.decls_by_module[static_cast<std::size_t>(decl_module)];
    auto it = decls.find(decl_name);
    if (it == decls.end()) {
        return false;
    }
    const Module& source = world.program->modules[static_cast<std::size_t>(decl_module)];
    return FormHead(source.tree.forms[it->second]) == head;
}

// the typed program with this declaration in it; false after the error
bool EnsureBuilt(MacroRun& run, Frame& frame, std::int32_t module, const std::string& name,
                 const Node& at) {
    MacroWorld& world = *run.world;
    if (!world.typed) {
        world.typed = std::make_shared<TypedCalls>();
    }
    TypedCalls& typed = *world.typed;
    if (typed.building) {
        RunFail(run, frame, at,
                name + " is used at compile time while the functions of another compile-time "
                       "call are being expanded; use it from a macro outside those functions");
        return false;
    }
    if (!HasRoot(typed, module, name) || !typed.built) {
        if (!HasRoot(typed, module, name)) {
            typed.roots.push_back({module, name});
        }
        typed.building = true;
        typed.program = Program();
        typed.layout = Layout();
        typed.consts.clear();
        typed.ok = BuildTyped(world, typed);
        typed.built = true;
        typed.building = false;
    }
    if (!typed.ok) {
        RunFail(run, frame, at,
                name + " can't be used at compile time until it checks: " + typed.failure);
        return false;
    }
    return true;
}

} // namespace

bool FindTypedConst(const MacroWorld& world, std::int32_t module, const std::string& name,
                    std::int32_t& const_module, std::string& const_name) {
    return FindDecl(world, module, name, "const", const_module, const_name);
}

Value ReadTypedConst(MacroRun& run, Frame& frame, std::int32_t const_module,
                     const std::string& const_name, const Node& at) {
    if (!EnsureBuilt(run, frame, const_module, const_name, at)) {
        return Value();
    }
    const TypedCalls& typed = *run.world->typed;
    for (const ConstInfo& info : typed.consts) {
        if (info.module != const_module || info.name != const_name || !info.ok) {
            continue;
        }
        TypeKind kind = KindOf(typed.program, info.type);
        if (kind == TypeKind::Bool) {
            return BoolValue(info.value != 0, frame.module);
        }
        if (kind == TypeKind::Ptr) {
            return IntValue(info.value, frame.module);
        }
        if (IsIntegerKind(kind)) {
            return IntValue(SignedValue(kind, info.value), frame.module);
        }
        RunFail(run, frame, at,
                const_name + " is a " + TypeName(typed.program, info.type) +
                    "; macro code reads integer and bool constants");
        return Value();
    }
    RunFail(run, frame, at, "internal: constant " + const_name + " is missing from its program");
    return Value();
}

bool FindTypedFunction(const MacroWorld& world, std::int32_t module, const std::string& name,
                       std::int32_t& fn_module, std::string& fn_name) {
    return FindDecl(world, module, name, "fn", fn_module, fn_name);
}

Value CallTyped(MacroRun& run, Frame& frame, std::int32_t fn_module, const std::string& fn_name,
                const std::vector<Value>& args, const Node& at) {
    if (!EnsureBuilt(run, frame, fn_module, fn_name, at)) {
        return Value();
    }
    MacroWorld& world = *run.world;
    TypedCalls& typed = *world.typed;
    const Program& program = typed.program;
    std::string qualified =
        world.program->modules[static_cast<std::size_t>(fn_module)].name + "." + fn_name;
    std::int32_t function = FindFunction(program, qualified);
    if (function < 0) {
        RunFail(run, frame, at, "internal: " + qualified + " is missing from its program");
        return Value();
    }
    const std::string& why = FunctionWhy(typed, function);
    if (!why.empty()) {
        RunFail(run, frame, at, fn_name + " can't run at compile time: it " + why);
        return Value();
    }
    const Function& fn = program.functions[static_cast<std::size_t>(function)];
    if (args.size() != fn.params.size()) {
        RunFail(run, frame, at,
                fn_name + " takes " + std::to_string(fn.params.size()) + " argument" +
                    (fn.params.size() == 1 ? "" : "s") + " " + Signature(program, fn) + ", got " +
                    std::to_string(args.size()));
        return Value();
    }
    std::vector<std::uint16_t> bits(args.size(), 0);
    for (std::size_t i = 0; i < args.size(); i++) {
        TypeId type = program.vars[static_cast<std::size_t>(fn.params[i])].type;
        std::string problem;
        if (!ArgBits(program, type, args[i], bits[i], problem)) {
            RunFail(run, frame, at, fn_name + " takes " + Signature(program, fn) + "; " + problem);
            return Value();
        }
    }
    typed.interp.max_steps = kMaxMacroSteps - std::min(run.steps, kMaxMacroSteps);
    InterpResult result = CallFunction(typed.interp, function, bits);
    if (!Step(run, result.steps)) {
        return Value();
    }
    if (result.kind != StopKind::Finished) {
        RunFail(run, frame, at,
                fn_name + " stopped at compile time: " + result.message + " at " +
                    FormatLoc(program.sources, result.loc));
        return Value();
    }
    if (!result.has_value) {
        return Value();
    }
    TypeKind kind = KindOf(program, result.type);
    if (kind == TypeKind::Bool) {
        return BoolValue(result.value != 0, frame.module);
    }
    if (kind == TypeKind::Ptr) {
        return IntValue(result.value, frame.module);
    }
    if (!IsIntegerKind(kind)) {
        RunFail(run, frame, at,
                fn_name + " returns " + TypeName(program, result.type) +
                    "; compile-time calls give integers and bools");
        return Value();
    }
    return IntValue(SignedValue(kind, result.value), frame.module);
}

} // namespace tisple
