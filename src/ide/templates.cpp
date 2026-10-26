#include "ide/index_internal.hpp"

#include <unordered_map>

namespace tisple {

namespace {

// what a top-level name in a module is
struct Entity {
    NameUseKind kind = NameUseKind::Function;
    std::int32_t index = -1;
};

using ModuleNames = std::unordered_map<std::string, Entity>;

struct TemplateWalk {
    IndexBuilder* ib = nullptr;
    std::vector<ModuleNames> modules;
    std::int32_t macro = -1;
    std::int32_t module = -1;
    std::uint32_t file = 0;
    std::unordered_map<std::string, std::int32_t> fresh; // tmp# -> definition
};

std::string AfterModule(const Program& program, std::int32_t module, const std::string& name) {
    const std::string& prefix = program.modules[static_cast<std::size_t>(module)].name;
    return name.size() > prefix.size() ? name.substr(prefix.size() + 1) : name;
}

// every module's top-level names, the way the checker's module scopes hold them
void CollectModuleNames(TemplateWalk& walk) {
    const Program& program = *walk.ib->program;
    const NameIndex& names = *walk.ib->names;
    walk.modules.resize(program.modules.size());
    auto add = [&](std::int32_t module, const std::string& name, NameUseKind kind,
                   std::size_t index) {
        if (module >= 0 && static_cast<std::size_t>(module) < walk.modules.size()) {
            walk.modules[static_cast<std::size_t>(module)].emplace(
                name, Entity{kind, static_cast<std::int32_t>(index)});
        }
    };
    for (std::size_t i = 0; i < program.functions.size(); i++) {
        const Function& fn = program.functions[i];
        add(fn.module, AfterModule(program, fn.module, fn.name), NameUseKind::Function, i);
    }
    for (std::size_t i = 0; i < program.vars.size(); i++) {
        const Var& var = program.vars[i];
        if (var.kind == VarKind::Global) {
            add(var.module, AfterModule(program, var.module, var.name), NameUseKind::Var, i);
        }
    }
    for (std::size_t i = 0; i < names.consts.size(); i++) {
        add(names.consts[i].module, names.consts[i].name, NameUseKind::Const, i);
    }
    for (std::size_t i = 0; i < program.structs.size(); i++) {
        add(program.structs[i].module, program.structs[i].name, NameUseKind::Struct, i);
    }
    for (std::size_t i = 0; i < program.enums.size(); i++) {
        add(program.enums[i].module, program.enums[i].name, NameUseKind::Enum, i);
    }
    for (std::size_t i = 0; i < names.macros.size(); i++) {
        add(names.macros[i].module, names.macros[i].name, NameUseKind::Macro, i);
    }
}

std::vector<std::string> Segments(const std::string& text) {
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (true) {
        std::size_t dot = text.find('.', start);
        std::string segment = text.substr(start, dot == std::string::npos ? dot : dot - start);
        if (segment.empty()) {
            return {};
        }
        segments.push_back(segment);
        if (dot == std::string::npos) {
            return segments;
        }
        start = dot + 1;
    }
}

void Note(TemplateWalk& walk, const Node& node, std::size_t segment, NameUseKind kind,
          std::int32_t index, std::int32_t member) {
    NoteNameUse(walk.ib->names, walk.ib->program->sources, walk.file, node, segment, kind, index,
                member, false);
}

// Name, or Name.member for an enum, starting at segment `first`
void NoteEntity(TemplateWalk& walk, const Node& node, const std::vector<std::string>& segments,
                std::size_t first, const Entity& entity) {
    Note(walk, node, first, entity.kind, entity.index, -1);
    if (entity.kind != NameUseKind::Enum || segments.size() <= first + 1) {
        return;
    }
    const EnumDecl& ed = walk.ib->program->enums[static_cast<std::size_t>(entity.index)];
    for (std::size_t i = 0; i < ed.members.size(); i++) {
        if (ed.members[i].name == segments[first + 1]) {
            Note(walk, node, first + 1, NameUseKind::EnumMember, entity.index,
                 static_cast<std::int32_t>(i));
        }
    }
}

// a name the template writes means what it means in the macro's own module
void ResolveName(TemplateWalk& walk, const Node& node) {
    std::vector<std::string> segments = Segments(node.text);
    if (segments.empty()) {
        return;
    }
    const ModuleNames& own = walk.modules[static_cast<std::size_t>(walk.module)];
    auto found = own.find(segments[0]);
    if (found != own.end()) {
        NoteEntity(walk, node, segments, 0, found->second);
        return;
    }
    const Module& module = walk.ib->loaded->modules[static_cast<std::size_t>(walk.module)];
    for (const Import& import : module.imports) {
        if (import.prefix != segments[0] || import.module < 0) {
            continue;
        }
        Note(walk, node, 0, NameUseKind::Module, import.module, -1);
        if (segments.size() < 2) {
            return;
        }
        const ModuleNames& target = walk.modules[static_cast<std::size_t>(import.module)];
        auto member = target.find(segments[1]);
        if (member != target.end()) {
            NoteEntity(walk, node, segments, 1, member->second);
        }
        return;
    }
}

// tmp#: one local per macro, declared where it first appears
void NoteFreshName(TemplateWalk& walk, const Node& node) {
    IndexBuilder& ib = *walk.ib;
    const std::string& text = node.text;
    const std::string& source = ib.program->sources.files[walk.file].text;
    std::uint32_t length = node.span.end - node.span.start;
    if (node.span.end > source.size() || length != text.size() ||
        source.compare(node.span.start, length, text) != 0) {
        return;
    }
    IndexToken token;
    token.loc.file = walk.file;
    token.loc.span = node.span;
    token.kind = SymbolKind::Local;
    auto known = walk.fresh.find(text);
    if (known == walk.fresh.end()) {
        Definition def;
        def.name = text;
        const std::string& module = ib.program->modules[static_cast<std::size_t>(walk.module)].name;
        def.qualified =
            module + "." + ib.names->macros[static_cast<std::size_t>(walk.macro)].name + "/" + text;
        def.kind = SymbolKind::Local;
        def.has_loc = true;
        def.loc = token.loc;
        def.type = "fresh name: a new " + text.substr(0, text.size() - 1) + "__n every expansion";
        known = walk.fresh.emplace(text, AddDefinition(ib, def)).first;
        token.declaration = true;
    }
    token.def = known->second;
    ib.out->tokens.push_back(token);
}

const std::string& Head(const Node& node) {
    static const std::string kEmpty;
    if (node.kind != NodeKind::List || node.items.empty() ||
        node.items[0].kind != NodeKind::Symbol) {
        return kEmpty;
    }
    return node.items[0].text;
}

void Walk(TemplateWalk& walk, const Node& node, bool unquoted) {
    if (node.kind == NodeKind::List) {
        const std::string& head = Head(node);
        bool unquote = (head == "unquote" || head == "unquote_splicing") && node.items.size() == 2;
        if (unquote) {
            Walk(walk, node.items[1], true);
            return;
        }
        for (const Node& item : node.items) {
            Walk(walk, item, unquoted);
        }
        return;
    }
    if (node.kind != NodeKind::Symbol) {
        return;
    }
    const MacroInfo& info = walk.ib->names->macros[static_cast<std::size_t>(walk.macro)];
    if (unquoted) {
        for (std::size_t i = 0; i < info.params.size(); i++) {
            if (info.params[i] == node.text) {
                Note(walk, node, 0, NameUseKind::MacroParam, walk.macro,
                     static_cast<std::int32_t>(i));
            }
        }
        return;
    }
    if (node.text.size() > 1 && node.text.back() == '#') {
        NoteFreshName(walk, node);
        return;
    }
    ResolveName(walk, node);
}

} // namespace

void IndexTemplates(IndexBuilder& ib) {
    TemplateWalk walk;
    walk.ib = &ib;
    CollectModuleNames(walk);
    for (std::size_t i = 0; i < ib.names->macros.size(); i++) {
        const MacroInfo& info = ib.names->macros[i];
        walk.macro = static_cast<std::int32_t>(i);
        walk.module = info.module;
        walk.file = info.loc.file;
        walk.fresh.clear();
        Walk(walk, info.body, false);
    }
}

} // namespace tisple
