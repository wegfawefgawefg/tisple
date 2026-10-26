#include "check/expand.hpp"

#include "check/names.hpp"

#include <unordered_map>

namespace tisple {

namespace {

struct MacroDef {
    std::string name;
    std::int32_t module = -1;
    std::vector<std::string> params;
    Node body; // the form inside the quasiquote
    Loc loc;
};

struct Expander {
    LoadedProgram* program = nullptr;
    std::vector<Diagnostic>* errors = nullptr;
    std::vector<MacroDef> macros;
    std::vector<std::unordered_map<std::string, std::int32_t>> by_module;
    std::uint32_t fresh_counter = 0;
    std::uint32_t expansions = 0;
    bool failed = false;
    NameIndex* names = nullptr; // editor tools only
};

// one expansion in progress
struct Instance {
    const MacroDef* def = nullptr;
    const std::vector<Node>* args = nullptr; // call items; args start at 1
    Span call_span;
    std::uint32_t expansion = 0;
    std::uint32_t file = 0;
    std::unordered_map<std::string, std::string> fresh;
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

std::string ParamList(const MacroDef& def) {
    std::string text;
    for (const std::string& param : def.params) {
        text += (text.empty() ? "" : " ") + param;
    }
    return text;
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
    if (!IsSnakeName(def.name)) {
        Fail(ex, LocOf(file, items[1]),
             "macro " + def.name + ": names are snake_case; write " + ToSnakeName(def.name));
        return false;
    }
    // (macro name (a b) `(...)); the old (args a b) or no list at all gets a fix
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
    for (const Node& param : params.items) {
        if (param.kind != NodeKind::Symbol || !IsSnakeName(param.text) ||
            IsBuiltinName(param.text)) {
            Fail(ex, LocOf(file, param),
                 "macro parameters are plain snake_case names; write (macro " + def.name +
                     " (a b) `(...))");
            return false;
        }
        def.params.push_back(param.text);
    }
    std::size_t next = 3;
    if (items.size() != next + 1) {
        Fail(ex, def.loc,
             "a macro body is one template; write (macro " + def.name + " (" + ParamList(def) +
                 ") `(...))");
        return false;
    }
    const Node& body = items[next];
    if (HeadText(body) != "quasiquote" || body.items.size() != 2) {
        Fail(ex, LocOf(file, body), "a macro body is one ` template; write `(...)");
        return false;
    }
    def.body = body.items[1];
    return true;
}

// the editor index: a macro, its name and its parameters, in the order ex.macros has them
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
    info.body = def.body;
    const Sources& sources = ex.program->sources;
    NoteNameUse(ex.names, sources, file, form.items[1], 0, NameUseKind::Macro, index, -1, true);
    const std::vector<Node>& params = form.items[2].items;
    for (std::size_t i = 0; i < params.size(); i++) {
        info.param_locs.push_back(LocOf(file, params[i]));
        NoteNameUse(ex.names, sources, file, params[i], 0, NameUseKind::MacroParam, index,
                    static_cast<std::int32_t>(i), true);
    }
    ex.names->macros.push_back(std::move(info));
}

void CollectMacros(Expander& ex) {
    std::size_t module_count = ex.program->modules.size();
    ex.by_module.resize(module_count);
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
            if (ex.by_module[m].count(def.name) != 0) {
                Fail(ex, def.loc, "macro " + def.name + " is defined twice; rename one");
                continue;
            }
            ex.by_module[m][def.name] = static_cast<std::int32_t>(ex.macros.size());
            module.macro_names.push_back(def.name);
            NoteMacro(ex, module.file, form, def);
            ex.macros.push_back(std::move(def));
        }
        module.tree.forms = std::move(kept);
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
    std::uint32_t expansion = form.items[0].expansion;
    if (expansion != 0) {
        module = ex.program->sources.expansions[expansion - 1].module;
    }
    std::int32_t target = module;
    std::string name = head;
    std::size_t dot = head.find('.');
    if (dot != std::string::npos) {
        target = -1;
        std::string prefix = head.substr(0, dot);
        name = head.substr(dot + 1);
        for (const Import& import : ex.program->modules[static_cast<std::size_t>(module)].imports) {
            if (import.prefix == prefix) {
                target = import.module;
            }
        }
        if (target < 0) {
            return nullptr;
        }
    }
    const auto& names = ex.by_module[static_cast<std::size_t>(target)];
    auto it = names.find(name);
    return it == names.end() ? nullptr : &ex.macros[static_cast<std::size_t>(it->second)];
}

bool IsMarkForm(const Node& node, const char* name) {
    return node.kind == NodeKind::List && node.items.size() == 2 && HeadText(node) == name;
}

Node Stamp(const Instance& in, const Node& node) {
    Node out = node;
    out.span = in.call_span;
    out.expansion = in.expansion;
    out.items.clear();
    return out;
}

bool Fill(Expander& ex, Instance& in, const Node& t, Node& out);

const Node* FindArg(const Instance& in, const Node& name) {
    if (name.kind != NodeKind::Symbol) {
        return nullptr;
    }
    for (std::size_t i = 0; i < in.def->params.size(); i++) {
        if (in.def->params[i] == name.text) {
            return &(*in.args)[i + 1];
        }
    }
    return nullptr;
}

Loc InstanceLoc(const Instance& in) {
    Loc loc;
    loc.file = in.file;
    loc.span = in.call_span;
    loc.expansion = in.expansion;
    return loc;
}

bool FillList(Expander& ex, Instance& in, const Node& t, Node& out) {
    out = Stamp(in, t);
    for (const Node& item : t.items) {
        if (!IsMarkForm(item, "unquote_splicing")) {
            Node filled;
            if (!Fill(ex, in, item, filled)) {
                return false;
            }
            out.items.push_back(std::move(filled));
            continue;
        }
        const Node* arg = FindArg(in, item.items[1]);
        if (!arg) {
            Fail(ex, InstanceLoc(in),
                 ",@ splices a macro argument; write ,@name with a name from (" +
                     ParamList(*in.def) + ")");
            return false;
        }
        if (arg->kind != NodeKind::List || arg->mark != QuoteMark::None) {
            Fail(ex, LocOf(in.file, *arg),
                 ",@" + item.items[1].text + " needs a list of forms; pass ((f a) (g b))");
            return false;
        }
        for (const Node& spliced : arg->items) {
            out.items.push_back(spliced);
        }
    }
    return true;
}

bool Fill(Expander& ex, Instance& in, const Node& t, Node& out) {
    if (IsMarkForm(t, "unquote")) {
        const Node* arg = FindArg(in, t.items[1]);
        if (!arg) {
            Fail(ex, InstanceLoc(in),
                 "only a macro argument can be unquoted; write ,name with a name from (" +
                     ParamList(*in.def) + ")");
            return false;
        }
        out = *arg;
        return true;
    }
    if (IsMarkForm(t, "unquote_splicing")) {
        Fail(ex, InstanceLoc(in), ",@ only works inside a list; write (do ,@body)");
        return false;
    }
    if (IsMarkForm(t, "quasiquote")) {
        Fail(ex, InstanceLoc(in), "a ` inside a template is not supported; use one ` per macro");
        return false;
    }
    if (t.kind == NodeKind::List) {
        return FillList(ex, in, t, out);
    }
    out = Stamp(in, t);
    if (t.kind == NodeKind::Symbol && t.text.size() > 1 && t.text.back() == '#') {
        auto it = in.fresh.find(t.text);
        if (it == in.fresh.end()) {
            ex.fresh_counter++;
            std::string base = t.text.substr(0, t.text.size() - 1);
            it = in.fresh.emplace(t.text, base + "__" + std::to_string(ex.fresh_counter)).first;
        }
        out.text = it->second;
    }
    return true;
}

bool Instantiate(Expander& ex, std::int32_t module, const MacroDef& def, const Node& call,
                 Node& out) {
    std::uint32_t file = ex.program->modules[static_cast<std::size_t>(module)].file;
    Loc call_loc = LocOf(file, call);
    std::size_t given = call.items.size() - 1;
    if (given != def.params.size()) {
        std::string want =
            "(" + call.items[0].text + (def.params.empty() ? "" : " ") + ParamList(def) + ")";
        Fail(ex, call_loc,
             def.name + " takes " + std::to_string(def.params.size()) + " argument" +
                 (def.params.size() == 1 ? "" : "s") + ", got " + std::to_string(given) +
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
    Instance in;
    in.def = &def;
    in.args = &call.items;
    in.call_span = call.span;
    in.expansion = static_cast<std::uint32_t>(sources.expansions.size());
    in.file = file;
    return Fill(ex, in, def.body, out);
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
    std::int32_t index = static_cast<std::int32_t>(&def - ex.macros.data());
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

} // namespace

bool ExpandProgram(LoadedProgram& program, std::vector<Diagnostic>& errors, NameIndex* names) {
    Expander ex;
    ex.program = &program;
    ex.errors = &errors;
    ex.names = names;
    CollectMacros(ex);
    for (std::size_t m = 0; m < program.modules.size(); m++) {
        for (Node& form : program.modules[m].tree.forms) {
            ExpandNode(ex, static_cast<std::int32_t>(m), form, 0);
        }
    }
    return !ex.failed;
}

} // namespace tisple
