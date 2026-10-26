#pragma once

// What the expander and checker found each written name to be, for editor tools (src/ide/).
// Filled only when CheckProgram and ExpandProgram are handed one; the compiler never reads it.
//
// A use is one segment of a symbol as written in a file: hgr.flip is two uses, the module
// hgr and the function flip. Names a macro template wrote are never recorded here (their
// span is the whole call); arguments keep their own spans, so they are.

#include "check/diag.hpp"
#include "check/program.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

enum class NameUseKind : std::uint8_t {
    Function,   // index: function id
    Var,        // index: var id (a top-level let, a parameter or a local)
    Const,      // index: into NameIndex::consts (scalars and tables)
    Struct,     // index: struct id
    Enum,       // index: enum id
    EnumMember, // index: enum id, member: member index
    Field,      // index: struct id, member: field index
    Module,     // index: module id
    Macro,      // index: into NameIndex::macros
    MacroParam, // index: into NameIndex::macros, member: parameter index
};

struct NameUse {
    NameUseKind kind = NameUseKind::Var;
    Loc loc; // just this segment
    std::int32_t index = -1;
    std::int32_t member = -1;
    bool declaration = false;
};

struct ConstInfo {
    std::string name;
    std::int32_t module = -1;
    Loc loc; // the name
    bool ok = false;
    TypeId type = kErrorType;
    std::uint16_t value = 0; // scalars
    std::int32_t var = -1;   // tables
};

struct MacroInfo {
    std::string name;
    std::int32_t module = -1;
    Loc loc;                         // the name
    Loc form;                        // the whole (macro ...) form
    std::vector<std::string> params; // without the &
    std::vector<Loc> param_locs;
    bool rest = false;      // the last parameter follows a &
    std::vector<Node> body; // the forms after the parameter list
};

struct NameIndex {
    std::vector<NameUse> uses;
    std::vector<ConstInfo> consts;
    std::vector<MacroInfo> macros;
};

// records segment `segment` of the symbol (or :keyword) node, but only when the node is
// written in file `file` as it reads, so template-made names stay out
void NoteNameUse(NameIndex* index, const Sources& sources, std::uint32_t file, const Node& node,
                 std::size_t segment, NameUseKind kind, std::int32_t target, std::int32_t member,
                 bool declaration);

} // namespace tisple
