#pragma once

// shared by the ide/*.cpp files; the public entry point is ide/index.hpp

#include "check/name_index.hpp"
#include "check/program.hpp"
#include "ide/index.hpp"

#include <map>
#include <string>
#include <tuple>

namespace tisple {

// (NameUseKind, index, member): one thing the checker or expander can name
using EntityKey = std::tuple<int, std::int32_t, std::int32_t>;

struct IndexBuilder {
    const LoadedProgram* loaded = nullptr; // trees after expansion, imports
    const Program* program = nullptr;
    NameIndex* names = nullptr;
    EditorIndex* out = nullptr;
    std::string lib_dir;
    std::map<EntityKey, std::int32_t> defs;
    std::map<EntityKey, Loc> declared; // where each entity's declaration use is
    std::map<std::string, std::int32_t> builtins;
};

EntityKey KeyOf(const NameUse& use);
std::int32_t AddDefinition(IndexBuilder& ib, const Definition& def);

// definitions (describe.cpp)
// the definition a use names, made the first time it is seen
std::int32_t DefinitionFor(IndexBuilder& ib, const NameUse& use);
SymbolKind UseSymbolKind(const IndexBuilder& ib, const NameUse& use);
// special forms, built-ins and primitive types by spelling; false for anything else
bool BuiltinKind(const std::string& text, SymbolKind& kind);
std::int32_t BuiltinDefinition(IndexBuilder& ib, const std::string& text, SymbolKind kind);
// leading (or trailing) comments of every definition; tokens must be sorted
void AddDocs(IndexBuilder& ib);
// the first lines of a file that are comments
std::string HeaderComment(const SourceFile& file);

// macro templates (templates.cpp): names in each macro's body, resolved in its module
void IndexTemplates(IndexBuilder& ib);

// outline (outline.cpp): top-level forms, read before expansion removes the macros
void BuildOutline(const LoadedProgram& loaded, std::vector<OutlineSymbol>& out);
// each outline entry's definition, from the declaration token at its name
void LinkOutline(EditorIndex& index);

// positions for JSON (json.cpp): 1-based line, 1-based column in UTF-16 code units
SourcePos EditorPos(const SourceFile& file, std::uint32_t offset);

} // namespace tisple
