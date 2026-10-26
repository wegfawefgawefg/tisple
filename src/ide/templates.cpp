#include "ide/index_internal.hpp"

#include <unordered_map>

namespace tisple {

namespace {

struct TemplateWalk {
    IndexBuilder* ib = nullptr;
    const std::vector<ModuleScopeMap>* scopes = nullptr;
    std::int32_t macro = -1;
    std::int32_t module = -1;
    std::uint32_t file = 0;
    std::unordered_map<std::string, std::int32_t> fresh; // tmp# -> definition
};

void Note(TemplateWalk& walk, const Node& node, std::size_t segment, NameUseKind kind,
          std::int32_t index, std::int32_t member) {
    NoteNameUse(walk.ib->names, walk.ib->program->sources, walk.file, node, segment, kind, index,
                member, false);
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

bool IsMarkForm(const Node& node, const char* name) {
    return node.kind == NodeKind::List && node.items.size() == 2 && Head(node) == name;
}

// code: macro code, which runs in the compiler; otherwise inside a ` template
void Walk(TemplateWalk& walk, const Node& node, bool code) {
    if (node.kind == NodeKind::List) {
        bool unquote = IsMarkForm(node, "unquote") || IsMarkForm(node, "unquote_splicing");
        if (!code && unquote) {
            Walk(walk, node.items[1], true);
            return;
        }
        if (code && IsMarkForm(node, "quasiquote")) {
            Walk(walk, node.items[1], false);
            return;
        }
        if (code && IsMarkForm(node, "quote")) {
            return;
        }
        for (const Node& item : node.items) {
            Walk(walk, item, code);
        }
        return;
    }
    if (node.kind != NodeKind::Symbol) {
        return;
    }
    const MacroInfo& info = walk.ib->names->macros[static_cast<std::size_t>(walk.macro)];
    if (code) {
        for (std::size_t i = 0; i < info.params.size(); i++) {
            if (info.params[i] == node.text) {
                Note(walk, node, 0, NameUseKind::MacroParam, walk.macro,
                     static_cast<std::int32_t>(i));
                return;
            }
        }
        // macros and functions macro code calls; its own lets resolve to nothing here
        ResolveWrittenName(*walk.ib, *walk.scopes, walk.module, walk.file, node);
        return;
    }
    if (node.text.size() > 1 && node.text.back() == '#') {
        NoteFreshName(walk, node);
        return;
    }
    // a name the template writes means what it means in the macro's own module
    ResolveWrittenName(*walk.ib, *walk.scopes, walk.module, walk.file, node);
}

} // namespace

void IndexTemplates(IndexBuilder& ib, const std::vector<ModuleScopeMap>& scopes) {
    TemplateWalk walk;
    walk.ib = &ib;
    walk.scopes = &scopes;
    for (std::size_t i = 0; i < ib.names->macros.size(); i++) {
        const MacroInfo& info = ib.names->macros[i];
        walk.macro = static_cast<std::int32_t>(i);
        walk.module = info.module;
        walk.file = info.loc.file;
        walk.fresh.clear();
        for (const Node& form : info.body) {
            Walk(walk, form, true);
        }
    }
}

} // namespace tisple
