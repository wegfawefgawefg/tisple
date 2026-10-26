#include "ide/index_internal.hpp"

#include <algorithm>

namespace tisple {

namespace {

std::string AfterModule(const Program& program, std::int32_t module, const std::string& name) {
    const std::string& prefix = program.modules[static_cast<std::size_t>(module)].name;
    return name.size() > prefix.size() ? name.substr(prefix.size() + 1) : name;
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

void Note(IndexBuilder& ib, std::uint32_t file, const Node& node, std::size_t segment,
          NameUseKind kind, std::int32_t index, std::int32_t member) {
    NoteNameUse(ib.names, ib.program->sources, file, node, segment, kind, index, member, false);
}

// dot.pos.x after a variable: each segment a field of the one before
void NoteFields(IndexBuilder& ib, std::uint32_t file, const Node& node,
                const std::vector<std::string>& segments, std::size_t first, TypeId type) {
    const Program& program = *ib.program;
    for (std::size_t i = first; i < segments.size(); i++) {
        if (type < 0 || static_cast<std::size_t>(type) >= program.types.size()) {
            return;
        }
        const Type& t = TypeOf(program, type);
        if (t.kind != TypeKind::Struct) {
            return;
        }
        const StructDecl& sd = program.structs[static_cast<std::size_t>(t.decl)];
        std::int32_t field = FindFieldIndex(program, type, segments[i]);
        if (field < 0) {
            return;
        }
        Note(ib, file, node, i, NameUseKind::Field, t.decl, field);
        type = sd.fields[static_cast<std::size_t>(field)].type;
    }
}

// Name, Name.member for an enum or name.field for a variable, from segment `first`
void NoteEntity(IndexBuilder& ib, std::uint32_t file, const Node& node,
                const std::vector<std::string>& segments, std::size_t first,
                const ScopeEntity& entity) {
    Note(ib, file, node, first, entity.kind, entity.index, -1);
    if (entity.kind == NameUseKind::Var) {
        TypeId type = ib.program->vars[static_cast<std::size_t>(entity.index)].type;
        NoteFields(ib, file, node, segments, first + 1, type);
        return;
    }
    if (entity.kind != NameUseKind::Enum || segments.size() <= first + 1) {
        return;
    }
    const EnumDecl& ed = ib.program->enums[static_cast<std::size_t>(entity.index)];
    for (std::size_t i = 0; i < ed.members.size(); i++) {
        if (ed.members[i].name == segments[first + 1]) {
            Note(ib, file, node, first + 1, NameUseKind::EnumMember, entity.index,
                 static_cast<std::int32_t>(i));
        }
    }
}

// ---- leftovers: names the checker never reached

struct Span2 {
    std::uint32_t file = 0;
    Span span;
};

bool SpanBefore(const Span2& a, const Span2& b) {
    if (a.file != b.file) {
        return a.file < b.file;
    }
    return a.span.start < b.span.start;
}

// the top-level form around offset, from the outline; false outside every one
bool EnclosingForm(const IndexBuilder& ib, std::uint32_t file, std::uint32_t offset, Span& out) {
    for (const OutlineSymbol& symbol : ib.out->symbols) {
        if (symbol.file == file && symbol.range.start <= offset && offset < symbol.range.end) {
            out = symbol.range;
            return true;
        }
    }
    return false;
}

// the last parameter or local of that name declared before offset in the same top-level form
std::int32_t EarlierLocal(const IndexBuilder& ib, std::uint32_t file, std::uint32_t offset,
                          const std::string& name) {
    Span form;
    if (!EnclosingForm(ib, file, offset, form)) {
        return -1;
    }
    const Program& program = *ib.program;
    const std::string& text = program.sources.files[file].text;
    std::int32_t found = -1;
    std::uint32_t found_at = 0;
    for (const NameUse& use : ib.names->uses) {
        if (!use.declaration || use.kind != NameUseKind::Var || use.loc.file != file ||
            use.loc.span.start < form.start || use.loc.span.start >= offset ||
            (found >= 0 && use.loc.span.start < found_at)) {
            continue;
        }
        VarKind kind = program.vars[static_cast<std::size_t>(use.index)].kind;
        std::uint32_t length = use.loc.span.end - use.loc.span.start;
        if ((kind == VarKind::Local || kind == VarKind::Param) && length == name.size() &&
            text.compare(use.loc.span.start, length, name) == 0) {
            found = use.index;
            found_at = use.loc.span.start;
        }
    }
    return found;
}

std::int32_t ModuleOfFile(const Program& program, std::uint32_t file) {
    for (std::size_t m = 0; m < program.modules.size(); m++) {
        if (program.modules[m].file == file) {
            return static_cast<std::int32_t>(m);
        }
    }
    return -1;
}

} // namespace

std::vector<ModuleScopeMap> CollectModuleScopes(const IndexBuilder& ib) {
    const Program& program = *ib.program;
    const NameIndex& names = *ib.names;
    std::vector<ModuleScopeMap> scopes(program.modules.size());
    auto add = [&](std::int32_t module, const std::string& name, NameUseKind kind,
                   std::size_t index) {
        if (module >= 0 && static_cast<std::size_t>(module) < scopes.size()) {
            scopes[static_cast<std::size_t>(module)].emplace(
                name, ScopeEntity{kind, static_cast<std::int32_t>(index)});
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
    return scopes;
}

void ResolveWrittenName(IndexBuilder& ib, const std::vector<ModuleScopeMap>& scopes,
                        std::int32_t module, std::uint32_t file, const Node& node) {
    std::vector<std::string> segments = Segments(node.text);
    if (segments.empty() || module < 0 || static_cast<std::size_t>(module) >= scopes.size()) {
        return;
    }
    const ModuleScopeMap& own = scopes[static_cast<std::size_t>(module)];
    auto found = own.find(segments[0]);
    if (found != own.end()) {
        NoteEntity(ib, file, node, segments, 0, found->second);
        return;
    }
    const Module& loaded = ib.loaded->modules[static_cast<std::size_t>(module)];
    for (const Import& import : loaded.imports) {
        if (import.prefix != segments[0] || import.module < 0) {
            continue;
        }
        Note(ib, file, node, 0, NameUseKind::Module, import.module, -1);
        if (segments.size() < 2) {
            return;
        }
        const ModuleScopeMap& target = scopes[static_cast<std::size_t>(import.module)];
        auto member = target.find(segments[1]);
        if (member != target.end()) {
            NoteEntity(ib, file, node, segments, 1, member->second);
        }
        return;
    }
}

void IndexLeftovers(IndexBuilder& ib, const std::vector<ModuleScopeMap>& scopes) {
    std::vector<Span2> covered;
    for (const NameUse& use : ib.names->uses) {
        covered.push_back(Span2{use.loc.file, use.loc.span});
    }
    for (const IndexToken& token : ib.out->tokens) {
        covered.push_back(Span2{token.loc.file, token.loc.span});
    }
    std::sort(covered.begin(), covered.end(), SpanBefore);
    const Sources& sources = ib.program->sources;
    for (std::uint32_t f = 0; f < sources.files.size(); f++) {
        const SourceFile& file = sources.files[f];
        std::int32_t module = ModuleOfFile(*ib.program, f);
        for (const Token& lexed : ib.lexed[f]) {
            if (lexed.kind != TokenKind::Symbol) {
                continue;
            }
            // anything noted inside this symbol already: the checker got here
            Span2 key{f, Span{lexed.span.start, lexed.span.start}};
            auto next = std::lower_bound(covered.begin(), covered.end(), key, SpanBefore);
            if (next != covered.end() && next->file == f && next->span.start < lexed.span.end) {
                continue;
            }
            Node node;
            node.kind = NodeKind::Symbol;
            node.span = lexed.span;
            node.text = file.text.substr(lexed.span.start, lexed.span.end - lexed.span.start);
            SymbolKind ignored_kind = SymbolKind::Builtin;
            if (BuiltinKind(node.text, ignored_kind) || node.text.back() == '#') {
                continue;
            }
            std::vector<std::string> segments = Segments(node.text);
            std::int32_t local =
                segments.empty() ? -1 : EarlierLocal(ib, f, lexed.span.start, segments[0]);
            if (local >= 0) {
                NoteEntity(ib, f, node, segments, 0, ScopeEntity{NameUseKind::Var, local});
                continue;
            }
            ResolveWrittenName(ib, scopes, module, f, node);
        }
    }
}

} // namespace tisple
