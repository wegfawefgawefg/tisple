#pragma once

// The editor index: what every name in every file of a program is, where it is declared and
// what type or signature it has, plus an outline per file and the errors. Built from load,
// macro expansion and the checker only (no layout, no backend), so it is quick enough to run
// on every keystroke, and best-effort: a program with errors still gives every token the
// checker could resolve. `tisple index` prints it as JSON for the VS Code extension.

#include "check/diag.hpp"
#include "check/load.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

enum class SymbolKind : std::uint8_t {
    Function,
    Macro,
    Parameter, // fn and macro parameters
    Local,     // locals, loop variables, fresh template names (tmp#)
    Global,
    Constant,
    StructType,
    EnumType,
    EnumMember,
    Field,
    Module,
    Builtin,
    SpecialForm,
    PrimitiveType,
};

// one thing a name can mean; built-ins have no file
struct Definition {
    std::string name;      // as declared: flip, Action.left, a2's module is apple2
    std::string qualified; // gfx/hgr.flip; the name for locals and built-ins
    SymbolKind kind = SymbolKind::Local;
    bool has_loc = false;
    Loc loc;              // the name in its declaration; a module's is the start of its file
    std::string type;     // u8, (ptr u8), fn flip ((x u16) (y u8)) void, Action.left = 1, ...
    std::string doc;      // the declaration's leading (or trailing) comment
    bool library = false; // built in, or declared in lib/
};

// one written name, or one segment of a dotted one
struct IndexToken {
    Loc loc;
    SymbolKind kind = SymbolKind::Local;
    std::int32_t def = -1;
    bool declaration = false;
};

// the outline: top-level forms per file, with fields and members inside
struct OutlineSymbol {
    std::string name;
    SymbolKind kind = SymbolKind::Function;
    std::uint32_t file = 0;
    Span range;     // the whole form
    Span name_span; // its name
    std::int32_t def = -1;
    std::vector<OutlineSymbol> children;
};

struct EditorIndex {
    Sources sources;
    std::vector<Definition> definitions;
    std::vector<IndexToken> tokens; // by file, then position
    std::vector<OutlineSymbol> symbols;
    std::vector<Diagnostic> errors;
};

// load (reading past syntax errors), expand and check entry_path and its imports
void IndexProgram(const std::string& entry_path, const LoadOptions& options, EditorIndex& out);

// The JSON tisple index prints. Lines and columns are 1-based, columns count UTF-16 code
// units (what editors use), and a range's end is exclusive. "file" indexes "files"; -1 is
// built in. "def" indexes "definitions"; -1 is none.
//
// {"files": ["/abs/main.tsp", ...],
//  "definitions": [{"name", "qualified", "kind", "type", "doc", "library", "file", "range"}],
//  "tokens": [{"file", "line", "column", "length", "kind", "def", "declaration"}],
//  "symbols": [{"name", "kind", "file", "range", "selection", "def", "children": [...]}],
//  "errors": [{"file", "range", "message", "macro"?}]}
//
// range: {"line", "column", "end_line", "end_column"}. kind: one of SymbolKindName's words.
std::string IndexJson(const EditorIndex& index);

const char* SymbolKindName(SymbolKind kind);

// tisple index file.tsp [--stdin]
int RunIndexCommand(int argc, char** argv);

} // namespace tisple
