#include "check/expand.hpp"

#include "check/names.hpp"
#include "macro/macro.hpp"

#include <unordered_map>

namespace tisple {

namespace {

struct Expander {
    MacroWorld world; // the macros, and what their code can see
    LoadedProgram* program = nullptr;
    std::vector<Diagnostic>* errors = nullptr;
    std::uint32_t expansions = 0;
    bool failed = false;
    NameIndex* names = nullptr; // editor tools only
};

void Fail(Expander& ex, const Loc& loc, const std::string& message) {
    AddError(*ex.errors, loc, message);
    ex.failed = true;
}

const std::string& HeadText(const Node& node) {
    static const std::string kEmpty;
    if (node.kind != NodeKind::List || node.items.empty() ||
        node.items[0].kind != NodeKind::Symbol) {
        return kEmpty;
    }
    return node.items[0].text;
}

// "a b & more"
std::string ParamList(const MacroDef& def) {
    std::string text;
    for (std::size_t i = 0; i < def.params.size(); i++) {
        bool rest = def.rest && i + 1 == def.params.size();
        text += (i == 0 ? "" : " ") + std::string(rest ? "& " : "") + def.params[i];
    }
    return text;
}

bool ReadParams(Expander& ex, std::uint32_t file, const Node& params, MacroDef& def) {
    const char* fix = "macro parameters are plain snake_case names; write (macro ";
    for (std::size_t i = 0; i < params.items.size(); i++) {
        const Node& param = params.items[i];
        // (a b & more): & comes once, right before the last name
        if (param.kind == NodeKind::Symbol && param.text == "&") {
            if (i + 2 != params.items.size() || def.rest) {
                Fail(ex, LocOf(file, param),
                     "& goes right before the last parameter, which takes the other arguments: "
                     "(macro " +
                         def.name + " (a & more) ...)");
                return false;
            }
            def.rest = true;
            continue;
        }
        if (param.kind != NodeKind::Symbol || !IsSnakeName(param.text) ||
            IsBuiltinName(param.text)) {
            Fail(ex, LocOf(file, param), fix + def.name + " (a b) `(...))");
            return false;
        }
        if (IsMacroLibraryName(param.text)) {
            Fail(ex, LocOf(file, param),
                 param.text + " already means something in macro code; rename the parameter");
            return false;
        }
        def.params.push_back(param.text);
    }
    return true;
}

bool ReadMacro(Expander& ex, std::int32_t module, const Node& form, MacroDef& def) {
    std::uint32_t file = ex.program->modules[static_cast<std::size_t>(module)].file;
    def.module = module;
    def.loc = LocOf(file, form);
    const std::vector<Node>& items = form.items;
    if (items.size() < 2 || items[1].kind != NodeKind::Symbol) {
        Fail(ex, def.loc, "a macro needs a name; write (macro name (a b) `(...))");
        return false;
    }
    def.name = items[1].text;
    if (IsBuiltinName(def.name)) {
        Fail(ex, LocOf(file, items[1]), def.name + " is a built-in name; rename the macro");
        return false;
    }
    if (IsMacroLibraryName(def.name)) {
        Fail(ex, LocOf(file, items[1]),
             def.name + " already means something in macro code; rename the macro");
        return false;
    }
    if (!IsSnakeName(def.name)) {
        Fail(ex, LocOf(file, items[1]),
             "macro " + def.name + ": names are snake_case; write " + ToSnakeName(def.name));
        return false;
    }
    // (macro name (a b) body ...); the old (args a b) or no list at all gets a fix
    const char* list_fix = "parameters are always a list now: write (macro ";
    if (items.size() == 3 && HeadText(items[2]) == "quasiquote") {
        Fail(ex, def.loc, list_fix + def.name + " () `(...))");
        return false;
    }
    if (items.size() < 3) {
        Fail(ex, def.loc,
             "a macro needs parameters and a template; write (macro " + def.name +
                 " (a b) `(...))");
        return false;
    }
    const Node& params = items[2];
    if (params.kind != NodeKind::List || params.mark != QuoteMark::None ||
        HeadText(params) == "args") {
        std::string names;
        for (std::size_t i = 1; HeadText(params) == "args" && i < params.items.size(); i++) {
            names += (i == 1 ? "" : " ") + params.items[i].text;
        }
        Fail(ex, LocOf(file, params), list_fix + def.name + " (" + names + ") `(...))");
        return false;
    }
    if (!ReadParams(ex, file, params, def)) {
        return false;
    }
    if (items.size() < 4) {
        Fail(ex, def.loc,
             "a macro needs a body; write (macro " + def.name + " (" + ParamList(def) +
                 ") `(...))");
        return false;
    }
    def.body.assign(items.begin() + 3, items.end());
    return true;
}

// the editor index: a macro, its name and its parameters, in the order the world has them
void NoteMacro(Expander& ex, std::uint32_t file, const Node& form, const MacroDef& def) {
    if (!ex.names) {
        return;
    }
    std::int32_t index = static_cast<std::int32_t>(ex.names->macros.size());
    MacroInfo info;
    info.name = def.name;
    info.module = def.module;
    info.loc = LocOf(file, form.items[1]);
    info.form = def.loc;
    info.params = def.params;
    info.rest = def.rest;
    info.body = def.body;
    const Sources& sources = ex.program->sources;
    NoteNameUse(ex.names, sources, file, form.items[1], 0, NameUseKind::Macro, index, -1, true);
    std::int32_t param_index = 0;
    for (const Node& param : form.items[2].items) {
        if (param.text == "&") {
            continue;
        }
        info.param_locs.push_back(LocOf(file, param));
        NoteNameUse(ex.names, sources, file, param, 0, NameUseKind::MacroParam, index, param_index,
                    true);
        param_index++;
    }
    ex.names->macros.push_back(std::move(info));
}

void CollectMacros(Expander& ex) {
    MacroWorld& world = ex.world;
    std::size_t module_count = ex.program->modules.size();
    world.macros_by_module.resize(module_count);
    for (std::size_t m = 0; m < module_count; m++) {
        Module& module = ex.program->modules[m];
        std::vector<Node> kept;
        for (Node& form : module.tree.forms) {
            if (HeadText(form) != "macro" || form.mark != QuoteMark::None) {
                kept.push_back(std::move(form));
                continue;
            }
            MacroDef def;
            if (!ReadMacro(ex, static_cast<std::int32_t>(m), form, def)) {
                continue;
            }
            if (world.macros_by_module[m].count(def.name) != 0) {
                Fail(ex, def.loc, "macro " + def.name + " is defined twice; rename one");
                continue;
            }
            world.macros_by_module[m][def.name] = static_cast<std::int32_t>(world.macros.size());
            module.macro_names.push_back(def.name);
            NoteMacro(ex, module.file, form, def);
            world.macros.push_back(std::move(def));
        }
        module.tree.forms = std::move(kept);
    }
}

// top-level declarations as written, for the functions macro code calls
void CollectDecls(Expander& ex) {
    MacroWorld& world = ex.world;
    world.decls_by_module.resize(ex.program->modules.size());
    for (std::size_t m = 0; m < ex.program->modules.size(); m++) {
        const std::vector<Node>& forms = ex.program->modules[m].tree.forms;
        for (std::size_t i = 0; i < forms.size(); i++) {
            const std::string& head = HeadText(forms[i]);
            bool decl = head == "fn" || head == "const" || head == "let" || head == "struct" ||
                        head == "enum";
            if (decl && forms[i].items.size() > 1 && forms[i].items[1].kind == NodeKind::Symbol) {
                world.decls_by_module[m].emplace(forms[i].items[1].text, i);
            }
        }
    }
}

// swap, or hgr.swap through an import of this module
const MacroDef* FindMacro(const Expander& ex, std::int32_t module, const Node& form) {
    if (form.kind != NodeKind::List || form.mark != QuoteMark::None) {
        return nullptr;
    }
    const std::string& head = HeadText(form);
    if (head.empty()) {
        return nullptr;
    }
    // a call a template wrote looks in the macro's own module
    return FindMacroByName(ex.world, ResolveModule(ex.world, form.items[0], module), head);
}

// the module names in an argument form resolve in
std::int32_t ArgModule(const Expander& ex, const Node& arg, std::int32_t module) {
    return ResolveModule(ex.world, arg, module);
}

bool Instantiate(Expander& ex, std::int32_t module, const MacroDef& def, const Node& call,
                 Node& out) {
    std::uint32_t file = ex.program->modules[static_cast<std::size_t>(module)].file;
    Loc call_loc = LocOf(file, call);
    std::size_t given = call.items.size() - 1;
    std::size_t fixed = def.rest ? def.params.size() - 1 : def.params.size();
    if (def.rest ? given < fixed : given != fixed) {
        std::string want =
            "(" + call.items[0].text + (def.params.empty() ? "" : " ") + ParamList(def) + ")";
        Fail(ex, call_loc,
             def.name + " takes " + (def.rest ? "at least " : "") + std::to_string(fixed) +
                 " argument" + (fixed == 1 ? "" : "s") + ", got " + std::to_string(given) +
                 "; write " + want);
        return false;
    }
    ex.expansions++;
    if (ex.expansions > kMaxExpansions) {
        Fail(ex, call_loc,
             "more than " + std::to_string(kMaxExpansions) +
                 " macro expansions; a macro probably expands to itself");
        return false;
    }
    Sources& sources = ex.program->sources;
    sources.expansions.push_back(Expansion{call.items[0].text, call_loc, def.module});
    MacroCall run;
    run.def = &def;
    run.call = call_loc;
    run.module = module;
    run.expansion = static_cast<std::uint32_t>(sources.expansions.size());
    for (std::size_t i = 1; i < call.items.size(); i++) {
        const Node& arg = call.items[i];
        run.args.push_back(NodeToValue(arg, true, file, ArgModule(ex, arg, module)));
    }
    if (!RunMacro(ex.world, run, out)) {
        ex.failed = true;
        return false;
    }
    return true;
}

void ExpandNode(Expander& ex, std::int32_t module, Node& node, int depth);

void ExpandItemsFrom(Expander& ex, std::int32_t module, Node& node, std::size_t first, int depth) {
    for (std::size_t i = first; i < node.items.size(); i++) {
        ExpandNode(ex, module, node.items[i], depth);
    }
}

// expand where forms are expected; skip names, types, fields, members and patterns
void ExpandChildren(Expander& ex, std::int32_t module, Node& node, int depth) {
    if (node.kind != NodeKind::List || node.items.empty() || node.mark != QuoteMark::None) {
        return;
    }
    const std::string& head = HeadText(node);
    if (head == "quote" || head == "quasiquote" || head == "unquote" ||
        head == "unquote_splicing" || head == "struct" || head == "enum" || head == "import" ||
        head == "macro") {
        return;
    }
    if (head == "fn") {
        // (fn name (params) type body ...): the parameter list holds no forms
        ExpandItemsFrom(ex, module, node, 3, depth);
        return;
    }
    if (head == "match") {
        if (node.items.size() > 1) {
            ExpandNode(ex, module, node.items[1], depth);
        }
        for (std::size_t i = 2; i < node.items.size(); i++) {
            Node& clause = node.items[i];
            if (clause.kind == NodeKind::List && clause.mark == QuoteMark::None) {
                ExpandItemsFrom(ex, module, clause, 1, depth);
            }
        }
        return;
    }
    if (head.empty()) {
        ExpandItemsFrom(ex, module, node, 0, depth);
        return;
    }
    ExpandItemsFrom(ex, module, node, 1, depth);
}

// the editor index: the call's head names the macro, after a module prefix if it has one
void NoteMacroCall(Expander& ex, std::int32_t module, const Node& call, const MacroDef& def) {
    if (!ex.names) {
        return;
    }
    const Node& head = call.items[0];
    std::uint32_t file = ex.program->modules[static_cast<std::size_t>(module)].file;
    const Sources& sources = ex.program->sources;
    std::size_t segment = 0;
    std::size_t dot = head.text.find('.');
    if (dot != std::string::npos) {
        std::string prefix = head.text.substr(0, dot);
        for (const Import& import : ex.program->modules[static_cast<std::size_t>(module)].imports) {
            if (import.prefix == prefix) {
                NoteNameUse(ex.names, sources, file, head, 0, NameUseKind::Module, import.module,
                            -1, false);
            }
        }
        segment = 1;
    }
    std::int32_t index = static_cast<std::int32_t>(&def - ex.world.macros.data());
    NoteNameUse(ex.names, sources, file, head, segment, NameUseKind::Macro, index, -1, false);
}

void ExpandNode(Expander& ex, std::int32_t module, Node& node, int depth) {
    while (const MacroDef* def = FindMacro(ex, module, node)) {
        NoteMacroCall(ex, module, node, *def);
        if (depth >= kMaxExpansionDepth) {
            std::uint32_t file = ex.program->modules[static_cast<std::size_t>(module)].file;
            Fail(ex, LocOf(file, node),
                 "macro " + def->name + " expands more than " + std::to_string(kMaxExpansionDepth) +
                     " levels deep; it probably expands to itself");
            return;
        }
        Node expanded;
        if (!Instantiate(ex, module, *def, node, expanded)) {
            return;
        }
        node = std::move(expanded);
        depth++;
    }
    ExpandChildren(ex, module, node, depth);
}

// a copy of a top-level form, for the program a typed compile-time call runs in: its errors
// go to errors, and the editor index never hears of it
bool ExpandCopy(Expander& ex, std::int32_t module, Node& form, std::vector<Diagnostic>& errors) {
    std::vector<Diagnostic>* saved_errors = ex.errors;
    NameIndex* saved_names = ex.names;
    bool saved_failed = ex.failed;
    ex.errors = &errors;
    ex.world.errors = &errors;
    ex.names = nullptr;
    ex.failed = false;
    ExpandNode(ex, module, form, 0);
    bool ok = !ex.failed;
    ex.errors = saved_errors;
    ex.world.errors = saved_errors;
    ex.names = saved_names;
    ex.failed = saved_failed;
    return ok;
}

} // namespace

bool ExpandProgram(LoadedProgram& program, std::vector<Diagnostic>& errors, NameIndex* names) {
    Expander ex;
    ex.program = &program;
    ex.errors = &errors;
    ex.names = names;
    ex.world.program = &program;
    ex.world.errors = &errors;
    ex.world.expand_form = [&ex](std::int32_t module, Node& form,
                                 std::vector<Diagnostic>& copy_errors) {
        return ExpandCopy(ex, module, form, copy_errors);
    };
    CollectMacros(ex);
    CollectDecls(ex);
    for (std::size_t m = 0; m < program.modules.size(); m++) {
        for (Node& form : program.modules[m].tree.forms) {
            ExpandNode(ex, static_cast<std::int32_t>(m), form, 0);
        }
    }
    return !ex.failed;
}

} // namespace tisple
