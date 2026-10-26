#include "ide/index.hpp"

#include "check/check.hpp"
#include "check/expand.hpp"
#include "ide/index_internal.hpp"

#include <algorithm>
#include <filesystem>

namespace tisple {

EntityKey KeyOf(const NameUse& use) {
    return EntityKey{static_cast<int>(use.kind), use.index, use.member};
}

std::int32_t AddDefinition(IndexBuilder& ib, const Definition& def) {
    ib.out->definitions.push_back(def);
    return static_cast<std::int32_t>(ib.out->definitions.size() - 1);
}

const char* SymbolKindName(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::Function:
        return "function";
    case SymbolKind::Macro:
        return "macro";
    case SymbolKind::Parameter:
        return "parameter";
    case SymbolKind::Local:
        return "local";
    case SymbolKind::Global:
        return "global";
    case SymbolKind::Constant:
        return "constant";
    case SymbolKind::StructType:
        return "struct";
    case SymbolKind::EnumType:
        return "enum";
    case SymbolKind::EnumMember:
        return "enum member";
    case SymbolKind::Field:
        return "field";
    case SymbolKind::Module:
        return "module";
    case SymbolKind::Builtin:
        return "built-in";
    case SymbolKind::SpecialForm:
        return "special form";
    case SymbolKind::PrimitiveType:
        return "primitive type";
    }
    return "?";
}

namespace {

// the checker and expander uses, templates' included, as tokens
void AddUseTokens(IndexBuilder& ib) {
    for (const NameUse& use : ib.names->uses) {
        if (use.declaration && ib.declared.count(KeyOf(use)) == 0) {
            ib.declared[KeyOf(use)] = use.loc;
        }
    }
    for (const NameUse& use : ib.names->uses) {
        IndexToken token;
        token.loc = use.loc;
        token.kind = UseSymbolKind(ib, use);
        token.def = DefinitionFor(ib, use);
        token.declaration = use.declaration;
        ib.out->tokens.push_back(token);
    }
}

// (import gfx/hgr g): the path and the prefix both name the module
void AddImportUses(IndexBuilder& ib) {
    for (const Module& module : ib.loaded->modules) {
        for (const Node& form : module.tree.forms) {
            if (form.kind != NodeKind::List || form.items.size() < 2 ||
                form.items[0].kind != NodeKind::Symbol || form.items[0].text != "import") {
                continue;
            }
            for (const Import& import : module.imports) {
                if (import.loc.span.start != form.span.start || import.module < 0) {
                    continue;
                }
                for (std::size_t i = 1; i < form.items.size() && i < 3; i++) {
                    NoteNameUse(ib.names, ib.program->sources, module.file, form.items[i], 0,
                                NameUseKind::Module, import.module, -1, false);
                }
            }
        }
    }
}

// special forms, built-ins and primitive types, straight from each file's tokens
void AddKeywordTokens(IndexBuilder& ib) {
    const Sources& sources = ib.program->sources;
    for (std::uint32_t f = 0; f < sources.files.size(); f++) {
        const SourceFile& file = sources.files[f];
        std::vector<Token> tokens;
        std::vector<ReadError> ignored;
        LexRecovering(file, tokens, ignored);
        for (const Token& lexed : tokens) {
            if (lexed.kind != TokenKind::Symbol) {
                continue;
            }
            std::string text =
                file.text.substr(lexed.span.start, lexed.span.end - lexed.span.start);
            SymbolKind kind = SymbolKind::Builtin;
            if (!BuiltinKind(text, kind)) {
                continue;
            }
            IndexToken token;
            token.loc.file = f;
            token.loc.span = lexed.span;
            token.kind = kind;
            token.def = BuiltinDefinition(ib, text, kind);
            ib.out->tokens.push_back(token);
        }
    }
}

bool TokenOrder(const IndexToken& a, const IndexToken& b) {
    if (a.loc.file != b.loc.file) {
        return a.loc.file < b.loc.file;
    }
    if (a.loc.span.start != b.loc.span.start) {
        return a.loc.span.start < b.loc.span.start;
    }
    return a.loc.span.end < b.loc.span.end;
}

bool SameSpan(const IndexToken& a, const IndexToken& b) {
    return a.loc.file == b.loc.file && a.loc.span.start == b.loc.span.start &&
           a.loc.span.end == b.loc.span.end;
}

// sorted, one token per span; the first one found wins (a macro argument used twice is
// checked twice)
void SortTokens(EditorIndex& index) {
    std::stable_sort(index.tokens.begin(), index.tokens.end(), TokenOrder);
    std::vector<IndexToken> kept;
    for (const IndexToken& token : index.tokens) {
        if (!kept.empty() && SameSpan(kept.back(), token)) {
            kept.back().declaration = kept.back().declaration || token.declaration;
            continue;
        }
        kept.push_back(token);
    }
    index.tokens = std::move(kept);
}

bool SameSpot(const Diagnostic& a, const Diagnostic& b) {
    return a.loc.file == b.loc.file && a.loc.span.start == b.loc.span.start &&
           a.loc.span.end == b.loc.span.end;
}

// one error per spot: a macro call that failed to expand also fails to check, and the
// expander's error is the one that says what to write
void KeepErrors(EditorIndex& index, const std::vector<Diagnostic>& errors) {
    for (const Diagnostic& error : errors) {
        bool repeated = false;
        for (const Diagnostic& kept : index.errors) {
            repeated = repeated || SameSpot(kept, error);
        }
        if (!repeated) {
            index.errors.push_back(error);
        }
    }
}

std::string NormalDir(const std::string& path) {
    std::string normal = std::filesystem::path(path).lexically_normal().string();
    while (normal.size() > 1 && normal.back() == '/') {
        normal.pop_back();
    }
    return normal;
}

} // namespace

void IndexProgram(const std::string& entry_path, const LoadOptions& options, EditorIndex& out) {
    out = EditorIndex();
    LoadOptions load_options = options;
    load_options.recover = true;
    LoadedProgram loaded;
    std::vector<Diagnostic> errors;
    LoadProgram(entry_path, load_options, loaded, errors);
    BuildOutline(loaded, out.symbols);

    NameIndex names;
    Program program;
    ExpandProgram(loaded, errors, &names);
    if (loaded.modules.empty()) {
        program.sources = std::move(loaded.sources);
    } else {
        CheckProgram(loaded, program, errors, &names);
    }

    IndexBuilder ib;
    ib.loaded = &loaded;
    ib.program = &program;
    ib.names = &names;
    ib.out = &out;
    ib.lib_dir = NormalDir(options.lib_dir.empty() ? DefaultLibDir() : options.lib_dir);
    out.sources = program.sources;
    if (out.sources.files.empty()) {
        // the entry could not be read: its errors still need a file to point at
        out.sources.files.push_back(SourceFileFromText(entry_path, ""));
    }
    IndexTemplates(ib);
    AddImportUses(ib);
    AddUseTokens(ib);
    AddKeywordTokens(ib);
    SortTokens(out);
    AddDocs(ib);
    LinkOutline(out);
    KeepErrors(out, errors);
}

} // namespace tisple
