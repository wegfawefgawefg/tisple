#include "check/check.hpp"

#include "check/checker.hpp"
#include "check/expand.hpp"

namespace tisple {

Loc NodeLoc(const Checker& ck, const Node& node) {
    return LocOf(ck.at.file, node);
}

void Err(Checker& ck, const Node& node, const std::string& message) {
    AddError(*ck.errors, NodeLoc(ck, node), message);
}

ExprId AddExpr(Checker& ck, ExprKind kind, TypeId type, const Node& node) {
    Expr expr;
    expr.kind = kind;
    expr.type = type;
    expr.loc = NodeLoc(ck, node);
    ck.out->exprs.push_back(expr);
    return static_cast<ExprId>(ck.out->exprs.size() - 1);
}

ExprId ErrorExpr(Checker& ck, const Node& node) {
    return AddExpr(ck, ExprKind::Literal, kErrorType, node);
}

ExprId Fail(Checker& ck, const Node& node, const std::string& message) {
    Err(ck, node, message);
    return ErrorExpr(ck, node);
}

void Note(Checker& ck, const Node& node, std::size_t segment, NameUseKind kind, std::int32_t target,
          std::int32_t member, bool declaration) {
    NoteNameUse(ck.names, ck.in->sources, ck.at.file, node, segment, kind, target, member,
                declaration);
}

void NoteField(Checker& ck, const Node& node, std::size_t segment, ExprId field_expr) {
    if (!ck.names || field_expr < 0) {
        return;
    }
    const Expr& expr = ExprAt(ck, field_expr);
    if (expr.kind != ExprKind::Field || expr.a < 0) {
        return;
    }
    const Type& base = TypeOf(*ck.out, ExprType(ck, expr.a));
    Note(ck, node, segment, NameUseKind::Field, base.decl, expr.ref, false);
}

Expr& ExprAt(Checker& ck, ExprId id) {
    return ck.out->exprs[static_cast<std::size_t>(id)];
}

TypeId ExprType(const Checker& ck, ExprId id) {
    return ck.out->exprs[static_cast<std::size_t>(id)].type;
}

std::int32_t AddVar(Checker& ck, const Var& var) {
    ck.out->vars.push_back(var);
    ck.global_forms.push_back(nullptr);
    ck.global_states.push_back(Progress::Done);
    return static_cast<std::int32_t>(ck.out->vars.size() - 1);
}

std::uint32_t AddList(Checker& ck, const std::vector<ExprId>& items) {
    std::uint32_t first = static_cast<std::uint32_t>(ck.out->lists.size());
    ck.out->lists.insert(ck.out->lists.end(), items.begin(), items.end());
    return first;
}

std::string Show(const Node& node) {
    std::string text;
    PrintFlatNode(node, text);
    return text;
}

std::string ShowType(const Checker& ck, TypeId type) {
    return TypeName(*ck.out, type);
}

const std::string& HeadName(const Node& node) {
    static const std::string kEmpty;
    if (node.kind != NodeKind::List || node.mark != QuoteMark::None || node.items.empty() ||
        node.items[0].kind != NodeKind::Symbol) {
        return kEmpty;
    }
    return node.items[0].text;
}

bool IsBareLiteral(const Node& node) {
    return node.kind == NodeKind::Integer;
}

Context SwitchContext(Checker& ck, std::int32_t module, std::int32_t function) {
    Context saved = std::move(ck.at);
    ck.at = Context();
    ck.at.module = module;
    ck.at.file = ck.in->modules[static_cast<std::size_t>(module)].file;
    ck.at.function = function;
    return saved;
}

void RestoreContext(Checker& ck, Context saved) {
    ck.at = std::move(saved);
}

namespace {

void InternBaseTypes(Checker& ck) {
    const TypeKind kinds[] = {TypeKind::Error, TypeKind::Void, TypeKind::Never, TypeKind::Bool,
                              TypeKind::U8,    TypeKind::I8,   TypeKind::U16,   TypeKind::I16};
    for (TypeKind kind : kinds) {
        Type type;
        type.kind = kind;
        type.size = kind == TypeKind::U16 || kind == TypeKind::I16 ? 2 : 1;
        if (kind == TypeKind::Error || kind == TypeKind::Void || kind == TypeKind::Never) {
            type.size = 0;
        }
        InternType(ck, type);
    }
}

bool IsParamType(const Checker& ck, TypeId type) {
    return type == kErrorType || IsScalarKind(KindOf(*ck.out, type));
}

// (params ...) holds only (name type) lists; anything else is the old shape or a body form
bool IsParamListShape(const Node& node) {
    if (node.kind != NodeKind::List || node.mark != QuoteMark::None) {
        return false;
    }
    for (const Node& item : node.items) {
        if (item.kind != NodeKind::List) {
            return false;
        }
    }
    return true;
}

// params[first..]: each (name type), added to the function and the current scope
void ReadParams(Checker& ck, Function& fn, std::int32_t index, const Node& params,
                std::size_t first) {
    for (std::size_t i = first; i < params.items.size(); i++) {
        const Node& param = params.items[i];
        if (param.kind != NodeKind::List || param.items.size() != 2 ||
            param.items[0].kind != NodeKind::Symbol) {
            Err(ck, param, "each parameter is (name type); write ((x u8) (y u8))");
            continue;
        }
        if (!CheckLocalName(ck, param.items[0], "parameter")) {
            continue;
        }
        TypeId type = ResolveType(ck, param.items[1]);
        CheckCallableName(ck, param.items[0], type, "parameter");
        if (!IsParamType(ck, type)) {
            Err(ck, param.items[1],
                "parameters are 1 or 2 bytes (integers, bool, enums, pointers); pass a (ptr " +
                    ShowType(ck, type) + ") instead");
            type = kErrorType;
        }
        Var var;
        var.name = param.items[0].text;
        var.kind = VarKind::Param;
        var.type = type;
        var.module = ck.at.module;
        var.function = index;
        var.loc = NodeLoc(ck, param.items[0]);
        std::int32_t id = AddVar(ck, var);
        Note(ck, param.items[0], 0, NameUseKind::Var, id, -1, true);
        fn.params.push_back(id);
        ck.at.scopes.back().entries.push_back(ScopeEntry{var.name, id});
    }
}

TypeId ReadReturnType(Checker& ck, const Node& node) {
    if (node.kind == NodeKind::Symbol && node.text == "void") {
        return kVoidType;
    }
    TypeId type = ResolveType(ck, node);
    if (!IsParamType(ck, type)) {
        Err(ck, node,
            "a function returns 1 or 2 bytes (integers, bool, enums, pointers); return a (ptr " +
                ShowType(ck, type) + ") instead");
        type = kErrorType;
    }
    return type;
}

// the return type slot holds void or a type; a body form there means it was left out
bool IsReturnTypeShape(Checker& ck, const Node& node) {
    if (node.kind == NodeKind::Symbol && node.text == "void") {
        return true;
    }
    if (node.kind == NodeKind::Symbol) {
        return TryResolveType(ck, node) >= 0;
    }
    const std::string& head = HeadName(node);
    return head == "ptr" || head == "array" || head == "fn";
}

std::string ShowItems(const Node& list, std::size_t first) {
    std::string text;
    for (std::size_t i = first; i < list.items.size(); i++) {
        text += (text.empty() ? "" : " ") + Show(list.items[i]);
    }
    return text;
}

// (fn name () void :interrupt ...): the program's interrupt handler
void ReadHandlerOption(Checker& ck, std::int32_t index, const Node& form, const Node& option,
                       const std::string& params_text, const std::string& return_text) {
    Function& fn = ck.out->functions[static_cast<std::size_t>(index)];
    const std::string& name = form.items[1].text;
    if (option.text != ":interrupt") {
        Err(ck, option, option.text + " is no fn option; the one there is is :interrupt");
        return;
    }
    if (!fn.params.empty() || fn.ret != kVoidType) {
        Err(ck, form,
            "an interrupt handler takes nothing and gives nothing; write (fn " + name +
                " () void :interrupt ...), not (fn " + name + " " + params_text + " " +
                return_text + " :interrupt ...)");
        return;
    }
    if (ck.out->handler >= 0) {
        Err(ck, option,
            "a program has one interrupt handler, and " +
                ck.out->functions[static_cast<std::size_t>(ck.out->handler)].name +
                " is it; poll every device there");
        return;
    }
    fn.interrupt = true;
    ck.out->handler = index;
}

// (fn name (params) type body ...); the old (args ...) and (returns t) get a fix
void ReadSignature(Checker& ck, std::int32_t index) {
    Progress& state = ck.signature_states[static_cast<std::size_t>(index)];
    if (state != Progress::Pending) {
        return;
    }
    state = Progress::Done;
    const Node& form = *ck.function_forms[static_cast<std::size_t>(index)];
    Function& fn = ck.out->functions[static_cast<std::size_t>(index)];
    Context saved = SwitchContext(ck, fn.module, index);
    ck.at.scopes.emplace_back();
    const std::vector<Node>& items = form.items;
    const std::string& name = items[1].text;
    std::size_t next = 2;
    bool old_params = false;
    bool old_returns = false;
    std::string params_text = "()";
    if (next < items.size() && HeadName(items[next]) == "args") {
        old_params = true;
        params_text = "(" + ShowItems(items[next], 1) + ")";
        ReadParams(ck, fn, index, items[next], 1);
        next++;
    } else if (next < items.size() && IsParamListShape(items[next])) {
        params_text = Show(items[next]);
        ReadParams(ck, fn, index, items[next], 0);
        next++;
    } else {
        old_params = true;
    }
    std::string return_text = "void";
    if (next < items.size() && HeadName(items[next]) == "returns") {
        old_returns = true;
        const Node& returns = items[next];
        if (returns.items.size() == 2) {
            return_text = Show(returns.items[1]);
            fn.ret = ReadReturnType(ck, returns.items[1]);
        }
        next++;
    } else if (!old_params && next < items.size() && IsReturnTypeShape(ck, items[next])) {
        return_text = Show(items[next]);
        fn.ret = ReadReturnType(ck, items[next]);
        next++;
    } else if (!old_params) {
        Err(ck, form,
            "the return type comes after the parameters, void when " + name +
                " returns nothing; write (fn " + name + " " + params_text + " void ...)");
    }
    // options after the return type: :interrupt
    if (next < items.size() && items[next].kind == NodeKind::Keyword) {
        ReadHandlerOption(ck, index, form, items[next], params_text, return_text);
        next++;
    }
    std::string fix = "(fn " + name + " " + params_text + " " + return_text + " ...)";
    if (old_params) {
        Err(ck, form, "parameters are always a list now: write " + fix);
    } else if (old_returns) {
        Err(ck, form, "the return type is always the third item now: write " + fix);
    }
    ck.body_starts[static_cast<std::size_t>(index)] = next;
    RestoreContext(ck, std::move(saved));
}

void CheckFunctionBody(Checker& ck, std::int32_t index) {
    const Node& form = *ck.function_forms[static_cast<std::size_t>(index)];
    Function& fn = ck.out->functions[static_cast<std::size_t>(index)];
    Context saved = SwitchContext(ck, fn.module, index);
    ck.at.scopes.emplace_back();
    for (std::int32_t param : fn.params) {
        const Var& var = ck.out->vars[static_cast<std::size_t>(param)];
        ck.at.scopes.back().entries.push_back(ScopeEntry{var.name, param});
    }
    std::size_t first = ck.body_starts[static_cast<std::size_t>(index)];
    TypeId ret = fn.ret;
    bool want = ret != kVoidType;
    ExprId body = CheckBody(ck, form.items, first, want ? ret : -1, want, form);
    ck.out->functions[static_cast<std::size_t>(index)].body = body;

    std::string short_name = form.items[1].text;
    TypeId got = ExprType(ck, body);
    if (want && got != ret && got != kNeverType && got != kErrorType && ret != kErrorType) {
        const Node& last = first < form.items.size() ? form.items.back() : form;
        if (got == kVoidType) {
            Err(ck, last,
                short_name + " returns " + ShowType(ck, ret) +
                    " but its last form gives no value; end the body with a " + ShowType(ck, ret) +
                    " value or (return v)");
        } else {
            RequireType(ck, last, body, ret, "the value of " + short_name);
        }
    }
    RestoreContext(ck, std::move(saved));
}

// depth-first over calls; a call back into the stack is recursion
void FindRecursion(Checker& ck, std::int32_t fn, std::vector<Progress>& state,
                   std::vector<std::int32_t>& stack) {
    state[static_cast<std::size_t>(fn)] = Progress::Busy;
    stack.push_back(fn);
    for (std::int32_t callee : ck.out->functions[static_cast<std::size_t>(fn)].callees) {
        Progress callee_state = state[static_cast<std::size_t>(callee)];
        if (callee_state == Progress::Pending) {
            FindRecursion(ck, callee, state, stack);
            continue;
        }
        if (callee_state != Progress::Busy) {
            continue;
        }
        const Function& target = ck.out->functions[static_cast<std::size_t>(callee)];
        std::string chain;
        bool started = false;
        bool indirect = false;
        for (std::int32_t on_stack : stack) {
            started = started || on_stack == callee;
            if (started && on_stack != callee) {
                chain += ck.out->functions[static_cast<std::size_t>(on_stack)].name + " -> ";
            }
            for (const auto& call : ck.indirect_calls) {
                indirect = indirect || (started && call.first == on_stack);
            }
        }
        std::string message = chain.empty()
                                  ? target.name + " calls itself"
                                  : target.name + " calls itself through " + chain + target.name;
        if (indirect) {
            message += " (a call through a function pointer may reach every function of its "
                       "type whose name is used as a value)";
        }
        AddError(*ck.errors, target.loc,
                 message + "; recursion is not in this subset, write it as a loop");
    }
    stack.pop_back();
    state[static_cast<std::size_t>(fn)] = Progress::Done;
}

} // namespace

void EnsureSignature(Checker& ck, std::int32_t function) {
    ReadSignature(ck, function);
}

namespace {

void FindMain(Checker& ck) {
    const ModuleScope& entry = ck.modules[0];
    auto it = entry.names.find("main");
    if (it == entry.names.end() || it->second.kind != DeclKind::Function) {
        return;
    }
    ck.out->main = it->second.index;
    const Function& fn = ck.out->functions[static_cast<std::size_t>(ck.out->main)];
    if (!fn.params.empty()) {
        AddError(*ck.errors, fn.loc, "main takes no parameters; write (fn main () void ...)");
    }
}

} // namespace

namespace {

// the editor index keeps what it needs of each constant
void CopyConsts(Checker& ck) {
    for (const ConstDecl& cd : ck.consts) {
        ConstInfo info;
        info.name = cd.name;
        info.module = cd.module;
        info.loc =
            LocOf(ck.in->modules[static_cast<std::size_t>(cd.module)].file, cd.form->items[1]);
        info.ok = cd.state == Progress::Done;
        info.type = cd.type;
        info.value = cd.value;
        info.var = cd.var;
        info.address = cd.address >= 0;
        ck.names->consts.push_back(info);
    }
}

} // namespace

bool CheckProgram(LoadedProgram& loaded, Program& out, std::vector<Diagnostic>& errors,
                  NameIndex* names) {
    out = Program();
    Checker ck;
    ck.in = &loaded;
    ck.out = &out;
    ck.errors = &errors;
    ck.names = names;
    std::size_t before = errors.size();
    InternBaseTypes(ck);
    for (const Module& module : loaded.modules) {
        out.modules.push_back(ModuleInfo{module.name, module.file});
    }

    // names first, everywhere, so order never matters
    ck.modules.resize(loaded.modules.size());
    for (std::size_t m = 0; m < loaded.modules.size(); m++) {
        DeclareModule(ck, static_cast<std::int32_t>(m));
    }
    for (std::size_t i = 0; i < out.structs.size(); i++) {
        Context saved = SwitchContext(ck, out.structs[i].module, -1);
        EnsureStruct(ck, static_cast<std::int32_t>(i), *ck.struct_forms[i]);
        RestoreContext(ck, std::move(saved));
    }
    // a const table that names a function reads its signature early
    ck.body_starts.resize(out.functions.size());
    ck.signature_states.assign(out.functions.size(), Progress::Pending);
    for (std::size_t i = 0; i < ck.consts.size(); i++) {
        EvalConst(ck, static_cast<std::int32_t>(i));
    }
    PlaceDiskConsts(ck);
    for (std::size_t i = 0; i < out.functions.size(); i++) {
        ReadSignature(ck, static_cast<std::int32_t>(i));
    }
    std::size_t var_count = out.vars.size();
    for (std::size_t i = 0; i < var_count; i++) {
        EnsureGlobal(ck, static_cast<std::int32_t>(i));
    }
    for (std::size_t i = 0; i < out.functions.size(); i++) {
        CheckFunctionBody(ck, static_cast<std::int32_t>(i));
    }
    CheckRegionUses(ck);
    AddIndirectCallees(ck);

    std::vector<Progress> state(out.functions.size(), Progress::Pending);
    std::vector<std::int32_t> stack;
    for (std::size_t i = 0; i < out.functions.size(); i++) {
        if (state[i] == Progress::Pending) {
            FindRecursion(ck, static_cast<std::int32_t>(i), state, stack);
        }
    }
    FindMain(ck);
    CheckHandler(ck);
    if (names) {
        CopyConsts(ck);
    }
    out.sources = std::move(loaded.sources);
    return errors.size() == before;
}

bool BuildProgram(const std::string& entry_path, const LoadOptions& options, Program& out,
                  std::vector<Diagnostic>& errors) {
    LoadedProgram loaded;
    bool ok = LoadProgram(entry_path, options, loaded, errors) && ExpandProgram(loaded, errors);
    if (!ok) {
        out = Program();
        out.sources = std::move(loaded.sources);
        return false;
    }
    return CheckProgram(loaded, out, errors);
}

} // namespace tisple
