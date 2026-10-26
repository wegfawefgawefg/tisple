#include "check/checker.hpp"
#include "check/names.hpp"

namespace tisple {

namespace {

std::string Qualified(const Checker& ck, const std::string& name) {
    return ck.out->modules[static_cast<std::size_t>(ck.at.module)].name + "." + name;
}

bool AddDecl(Checker& ck, const Node& name, DeclKind kind, std::int32_t index) {
    ModuleScope& scope = ck.modules[static_cast<std::size_t>(ck.at.module)];
    auto it = scope.names.find(name.text);
    if (it != scope.names.end()) {
        std::string first = it->second.kind == DeclKind::Macro
                                ? "a macro"
                                : "first at " + FormatLoc(ck.in->sources, it->second.loc);
        Err(ck, name, name.text + " is declared twice in this module (" + first + "); rename one");
        return false;
    }
    scope.names[name.text] = Decl{kind, index, NodeLoc(ck, name)};
    return true;
}

void DeclareImports(Checker& ck, std::int32_t module) {
    ModuleScope& scope = ck.modules[static_cast<std::size_t>(module)];
    for (const Import& import : ck.in->modules[static_cast<std::size_t>(module)].imports) {
        if (IsBuiltinName(import.prefix) || !IsSnakeName(import.prefix)) {
            AddError(*ck.errors, import.loc,
                     "import prefix " + import.prefix +
                         " must be a snake_case name that is not built in; write (import " +
                         import.path + " " + ToSnakeName(import.prefix + "_mod") + ")");
            continue;
        }
        scope.imports[import.prefix] = import.module;
    }
    for (const std::string& macro : ck.in->modules[static_cast<std::size_t>(module)].macro_names) {
        scope.names[macro] = Decl{DeclKind::Macro, -1, Loc{}};
    }
}

void DeclareEnum(Checker& ck, const Node& form) {
    const Node& name = form.items[1];
    if (!CheckNewName(ck, name, NameRule::Type, "enum")) {
        return;
    }
    EnumDecl ed;
    ed.name = name.text;
    ed.module = ck.at.module;
    ed.loc = NodeLoc(ck, form);
    // (enum Action (none left right)); the old (enum Action none left right) gets a fix
    const std::vector<Node>* items = &form.items;
    std::size_t first = 2;
    if (form.items.size() == 3 && form.items[2].kind == NodeKind::List &&
        form.items[2].mark == QuoteMark::None) {
        items = &form.items[2].items;
        first = 0;
        const std::vector<Node>& list = *items;
        if (list.size() == 2 && list[0].kind == NodeKind::Symbol &&
            list[1].kind == NodeKind::Integer) {
            Err(ck, form,
                "a member with a value is (name number) inside the list; write (enum " + ed.name +
                    " ((" + list[0].text + " " + list[1].text + ")))");
            return;
        }
    } else if (form.items.size() > 2) {
        std::string list;
        for (std::size_t i = 2; i < form.items.size(); i++) {
            list += (i == 2 ? "" : " ") + Show(form.items[i]);
        }
        Err(ck, form,
            "members are one list, like parameters: write (enum " + ed.name + " (" + list + "))");
    }
    if (items->size() <= first) {
        Err(ck, form, "an enum needs members; write (enum " + ed.name + " (first second))");
    }
    std::uint32_t next = 0;
    for (std::size_t i = first; i < items->size(); i++) {
        const Node& item = (*items)[i];
        const Node* member_name = &item;
        std::uint32_t value = next;
        if (item.kind == NodeKind::List) {
            if (item.items.size() != 2 || item.items[1].kind != NodeKind::Integer) {
                Err(ck, item, "a member with a value is (name number); write (fire 7)");
                continue;
            }
            member_name = &item.items[0];
            if (item.items[1].value < 0 || item.items[1].value > 255) {
                Err(ck, item.items[1], "enum values are stored as u8; use 0 to 255");
                continue;
            }
            value = static_cast<std::uint32_t>(item.items[1].value);
        }
        if (!CheckNewName(ck, *member_name, NameRule::Snake, "member")) {
            continue;
        }
        if (value > 255) {
            Err(ck, *member_name,
                "member " + member_name->text + " would be " + std::to_string(value) +
                    "; enum values are stored as u8, so give it a value from 0 to 255");
            continue;
        }
        bool clash = false;
        for (const EnumMember& other : ed.members) {
            if (other.name == member_name->text) {
                Err(ck, *member_name, ed.name + "." + other.name + " appears twice; rename one");
                clash = true;
            } else if (other.value == value) {
                Err(ck, *member_name,
                    ed.name + "." + member_name->text + " and " + ed.name + "." + other.name +
                        " both have value " + std::to_string(value) +
                        "; give each member its own value");
                clash = true;
            }
        }
        if (clash) {
            continue;
        }
        ed.members.push_back(EnumMember{member_name->text, static_cast<std::uint8_t>(value)});
        next = value + 1;
    }
    std::int32_t index = static_cast<std::int32_t>(ck.out->enums.size());
    ck.out->enums.push_back(ed);
    EnumType(ck, index);
    AddDecl(ck, name, DeclKind::Enum, index);
}

void DeclareStruct(Checker& ck, const Node& form) {
    const Node& name = form.items[1];
    if (!CheckNewName(ck, name, NameRule::Type, "struct")) {
        return;
    }
    StructDecl sd;
    sd.name = name.text;
    sd.module = ck.at.module;
    sd.loc = NodeLoc(ck, form);
    std::int32_t index = static_cast<std::int32_t>(ck.out->structs.size());
    ck.out->structs.push_back(sd);
    ck.struct_forms.push_back(&form);
    ck.struct_states.push_back(Progress::Pending);
    StructType(ck, index);
    AddDecl(ck, name, DeclKind::Struct, index);
}

void DeclareConst(Checker& ck, const Node& form) {
    const Node& name = form.items[1];
    if (!CheckNewName(ck, name, NameRule::Constant, "const")) {
        return;
    }
    ConstDecl cd;
    cd.name = name.text;
    cd.module = ck.at.module;
    cd.form = &form;
    ck.consts.push_back(cd);
    AddDecl(ck, name, DeclKind::Const, static_cast<std::int32_t>(ck.consts.size() - 1));
}

void DeclareGlobal(Checker& ck, const Node& form) {
    const Node& name = form.items[1];
    if (!CheckNewName(ck, name, NameRule::Snake, "let")) {
        return;
    }
    Var var;
    var.name = Qualified(ck, name.text);
    var.kind = VarKind::Global;
    var.type = kErrorType;
    var.module = ck.at.module;
    var.loc = NodeLoc(ck, name);
    std::int32_t id = AddVar(ck, var);
    ck.global_forms[static_cast<std::size_t>(id)] = &form;
    AddDecl(ck, name, DeclKind::Var, id);
}

void DeclareFunction(Checker& ck, const Node& form) {
    const Node& name = form.items[1];
    if (!CheckNewName(ck, name, NameRule::Snake, "fn")) {
        return;
    }
    Function fn;
    fn.name = Qualified(ck, name.text);
    fn.module = ck.at.module;
    fn.loc = NodeLoc(ck, form);
    std::int32_t index = static_cast<std::int32_t>(ck.out->functions.size());
    ck.out->functions.push_back(fn);
    ck.function_forms.push_back(&form);
    AddDecl(ck, name, DeclKind::Function, index);
}

} // namespace

void DeclareModule(Checker& ck, std::int32_t module) {
    Context saved = SwitchContext(ck, module, -1);
    DeclareImports(ck, module);
    for (const Node& form : ck.in->modules[static_cast<std::size_t>(module)].tree.forms) {
        const std::string& head = HeadName(form);
        if (head == "import") {
            continue;
        }
        bool known =
            head == "struct" || head == "enum" || head == "const" || head == "let" || head == "fn";
        if (!known) {
            Err(ck, form,
                "only import, const, let, struct, enum, fn and macro go at top level; put " +
                    (head.empty() ? Show(form) : "(" + head + " ...)") + " inside a fn");
            continue;
        }
        if (form.items.size() < 2 || form.items[1].kind != NodeKind::Symbol) {
            Err(ck, form, head + " needs a name; write (" + head + " name ...)");
            continue;
        }
        if (head == "struct") {
            DeclareStruct(ck, form);
        } else if (head == "enum") {
            DeclareEnum(ck, form);
        } else if (head == "const") {
            DeclareConst(ck, form);
        } else if (head == "let") {
            DeclareGlobal(ck, form);
        } else {
            DeclareFunction(ck, form);
        }
    }
    RestoreContext(ck, std::move(saved));
}

} // namespace tisple
