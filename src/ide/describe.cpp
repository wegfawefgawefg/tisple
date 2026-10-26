#include "ide/index_internal.hpp"

#include <cstdio>

namespace tisple {

namespace {

// ---- type strings

std::string Hex(std::uint32_t value, int digits) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%0*X", digits, value);
    return text;
}

// a constant's value as it would be written
std::string ValueText(const Program& program, TypeId type, std::uint16_t raw) {
    const Type& t = TypeOf(program, type);
    if (t.kind == TypeKind::Bool) {
        return raw != 0 ? "true" : "false";
    }
    if (t.kind == TypeKind::Enum) {
        const EnumDecl& ed = program.enums[static_cast<std::size_t>(t.decl)];
        for (const EnumMember& member : ed.members) {
            if (member.value == raw) {
                return ed.name + "." + member.name;
            }
        }
        return std::to_string(raw);
    }
    if (t.kind == TypeKind::I8) {
        return std::to_string(static_cast<std::int8_t>(raw & 0xFF));
    }
    if (t.kind == TypeKind::I16) {
        return std::to_string(static_cast<std::int16_t>(raw));
    }
    int digits = KindBits(t.kind) == 16 ? 4 : 2;
    if (raw >= 256) {
        return Hex(raw, digits) + " (" + std::to_string(raw) + ")";
    }
    if (raw >= 10) {
        return std::to_string(raw) + " (" + Hex(raw, digits) + ")";
    }
    return std::to_string(raw);
}

std::string ShortName(const Program& program, std::int32_t module, const std::string& name) {
    if (module < 0) {
        return name;
    }
    const std::string& prefix = program.modules[static_cast<std::size_t>(module)].name;
    if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
        name[prefix.size()] == '.') {
        return name.substr(prefix.size() + 1);
    }
    return name;
}

std::string Signature(const Program& program, const Function& fn) {
    std::string params;
    for (std::int32_t id : fn.params) {
        const Var& var = program.vars[static_cast<std::size_t>(id)];
        params +=
            (params.empty() ? "(" : " (") + var.name + " " + TypeName(program, var.type) + ")";
    }
    std::string ret = fn.ret == kVoidType ? "void" : TypeName(program, fn.ret);
    return "fn " + ShortName(program, fn.module, fn.name) + " (" + params + ") " + ret;
}

std::string StructText(const Program& program, const StructDecl& sd) {
    std::string fields;
    for (const Field& field : sd.fields) {
        fields +=
            (fields.empty() ? "(" : " (") + field.name + " " + TypeName(program, field.type) + ")";
    }
    return "struct " + sd.name + " (" + fields + ")";
}

std::string EnumText(const EnumDecl& ed) {
    bool counted = true;
    for (std::size_t i = 0; i < ed.members.size(); i++) {
        counted = counted && ed.members[i].value == i;
    }
    std::string members;
    for (const EnumMember& member : ed.members) {
        members += members.empty() ? "" : " ";
        members +=
            counted ? member.name : "(" + member.name + " " + std::to_string(member.value) + ")";
    }
    return "enum " + ed.name + " (" + members + ")";
}

SymbolKind VarSymbolKind(VarKind kind) {
    switch (kind) {
    case VarKind::Global:
        return SymbolKind::Global;
    case VarKind::ConstArray:
        return SymbolKind::Constant;
    case VarKind::Param:
        return SymbolKind::Parameter;
    case VarKind::Local:
        break;
    }
    return SymbolKind::Local;
}

bool InLib(const IndexBuilder& ib, std::uint32_t file) {
    const std::string& path = ib.program->sources.files[file].path;
    const std::string& lib = ib.lib_dir;
    return !lib.empty() && path.size() > lib.size() && path.compare(0, lib.size(), lib) == 0 &&
           path[lib.size()] == '/';
}

std::string ModuleOf(const IndexBuilder& ib, std::int32_t module) {
    return module < 0 ? "" : ib.program->modules[static_cast<std::size_t>(module)].name;
}

// name, qualified name, kind, type and fallback location of what a use names
Definition Describe(const IndexBuilder& ib, const NameUse& use) {
    const Program& program = *ib.program;
    std::size_t i = static_cast<std::size_t>(use.index);
    Definition def;
    def.kind = UseSymbolKind(ib, use);
    switch (use.kind) {
    case NameUseKind::Function: {
        const Function& fn = program.functions[i];
        def.name = ShortName(program, fn.module, fn.name);
        def.qualified = fn.name;
        def.type = Signature(program, fn);
        def.loc = fn.loc;
        break;
    }
    case NameUseKind::Var: {
        const Var& var = program.vars[i];
        def.name =
            var.kind == VarKind::Global ? ShortName(program, var.module, var.name) : var.name;
        def.qualified = var.name;
        if (var.function >= 0) {
            const Function& fn = program.functions[static_cast<std::size_t>(var.function)];
            def.qualified = fn.name + "/" + var.name;
        }
        def.type = TypeName(program, var.type);
        def.loc = var.loc;
        break;
    }
    case NameUseKind::Const: {
        const ConstInfo& info = ib.names->consts[i];
        def.name = info.name;
        def.qualified = ModuleOf(ib, info.module) + "." + info.name;
        def.type = TypeName(program, info.type);
        if (info.ok && info.var < 0) {
            def.type += " = " + ValueText(program, info.type, info.value);
        }
        def.loc = info.loc;
        break;
    }
    case NameUseKind::Struct: {
        const StructDecl& sd = program.structs[i];
        def.name = sd.name;
        def.qualified = ModuleOf(ib, sd.module) + "." + sd.name;
        def.type = StructText(program, sd);
        def.loc = sd.loc;
        break;
    }
    case NameUseKind::Enum: {
        const EnumDecl& ed = program.enums[i];
        def.name = ed.name;
        def.qualified = ModuleOf(ib, ed.module) + "." + ed.name;
        def.type = EnumText(ed);
        def.loc = ed.loc;
        break;
    }
    case NameUseKind::EnumMember: {
        const EnumDecl& ed = program.enums[i];
        const EnumMember& member = ed.members[static_cast<std::size_t>(use.member)];
        def.name = ed.name + "." + member.name;
        def.qualified = ModuleOf(ib, ed.module) + "." + def.name;
        def.type = def.name + " = " + std::to_string(member.value);
        def.loc = ed.loc;
        break;
    }
    case NameUseKind::Field: {
        const StructDecl& sd = program.structs[i];
        const Field& field = sd.fields[static_cast<std::size_t>(use.member)];
        def.name = field.name;
        def.qualified = ModuleOf(ib, sd.module) + "." + sd.name + "." + field.name;
        def.type = "field " + field.name + " " + TypeName(program, field.type) + " of " + sd.name;
        def.loc = sd.loc;
        break;
    }
    case NameUseKind::Module: {
        const ModuleInfo& module = program.modules[i];
        def.name = module.name;
        def.qualified = module.name;
        def.type = "module " + module.name;
        def.loc.file = module.file;
        def.doc = HeaderComment(program.sources.files[module.file]);
        break;
    }
    case NameUseKind::Macro: {
        const MacroInfo& info = ib.names->macros[i];
        std::string params;
        for (std::size_t p = 0; p < info.params.size(); p++) {
            bool rest = info.rest && p + 1 == info.params.size();
            params += (params.empty() ? "" : " ") + std::string(rest ? "& " : "") + info.params[p];
        }
        def.name = info.name;
        def.qualified = ModuleOf(ib, info.module) + "." + info.name;
        def.type = "macro " + info.name + " (" + params + ")";
        def.loc = info.loc;
        break;
    }
    case NameUseKind::MacroParam: {
        const MacroInfo& info = ib.names->macros[i];
        def.name = info.params[static_cast<std::size_t>(use.member)];
        def.qualified = ModuleOf(ib, info.module) + "." + info.name + "/" + def.name;
        def.type = "parameter " + def.name + " of macro " + info.name;
        def.loc = info.param_locs[static_cast<std::size_t>(use.member)];
        break;
    }
    }
    return def;
}

// ---- comments

std::string LineText(const SourceFile& file, std::size_t line) {
    std::uint32_t start = file.line_starts[line];
    std::uint32_t end = line + 1 < file.line_starts.size()
                            ? file.line_starts[line + 1] - 1
                            : static_cast<std::uint32_t>(file.text.size());
    return file.text.substr(start, end - start);
}

// "; text" -> "text"; empty when the line is not only a comment
bool CommentLine(const std::string& line, std::string& text) {
    std::size_t at = line.find_first_not_of(" \t\r");
    if (at == std::string::npos || line[at] != ';') {
        return false;
    }
    while (at < line.size() && line[at] == ';') {
        at++;
    }
    if (at < line.size() && line[at] == ' ') {
        at++;
    }
    text = line.substr(at);
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return true;
}

std::string LeadingComment(const SourceFile& file, std::size_t line) {
    std::string doc;
    std::string text;
    while (line > 0 && CommentLine(LineText(file, line - 1), text)) {
        doc = doc.empty() ? text : text + "\n" + doc;
        line--;
    }
    return doc;
}

// a ; comment after offset on its line, outside strings; npos when there is none
std::size_t TrailingCommentStart(const SourceFile& file, std::uint32_t offset) {
    bool in_string = false;
    for (std::size_t i = offset; i < file.text.size() && file.text[i] != '\n'; i++) {
        char c = file.text[i];
        if (in_string && c == '\\') {
            i++;
        } else if (c == '"') {
            in_string = !in_string;
        } else if (c == ';' && !in_string) {
            return i;
        }
    }
    return std::string::npos;
}

// comments above a line belong to its first declaration: flip, not its parameters
std::string LeadingDoc(const EditorIndex& index, const Definition& def, std::size_t token) {
    const SourceFile& file = index.sources.files[def.loc.file];
    SourcePos pos = PosFromOffset(file, def.loc.span.start);
    std::uint32_t line_start = file.line_starts[pos.line - 1];
    for (std::size_t t = token; t > 0; t--) {
        const IndexToken& before = index.tokens[t - 1];
        if (before.loc.file != def.loc.file || before.loc.span.start < line_start) {
            break;
        }
        if (before.declaration) {
            return "";
        }
    }
    return LeadingComment(file, pos.line - 1);
}

// a comment at the end of a line belongs to the last declaration before it
std::string TrailingDoc(const EditorIndex& index, const Definition& def, std::size_t token) {
    const SourceFile& file = index.sources.files[def.loc.file];
    std::size_t start = TrailingCommentStart(file, def.loc.span.end);
    if (start == std::string::npos) {
        return "";
    }
    for (std::size_t t = token + 1; t < index.tokens.size(); t++) {
        const IndexToken& next = index.tokens[t];
        if (next.loc.file != def.loc.file || next.loc.span.start >= start) {
            break;
        }
        if (next.declaration) {
            return "";
        }
    }
    std::string text;
    std::size_t end = file.text.find('\n', start);
    CommentLine(file.text.substr(start, end == std::string::npos ? end : end - start), text);
    return text;
}

// both when there are both: a section comment above, a note after the name
std::string DocFor(const EditorIndex& index, const Definition& def, std::size_t token) {
    std::string leading = LeadingDoc(index, def, token);
    std::string trailing = TrailingDoc(index, def, token);
    if (leading.empty() || trailing.empty()) {
        return leading + trailing;
    }
    return leading + "\n\n" + trailing;
}

} // namespace

SymbolKind UseSymbolKind(const IndexBuilder& ib, const NameUse& use) {
    switch (use.kind) {
    case NameUseKind::Function:
        return SymbolKind::Function;
    case NameUseKind::Var:
        return VarSymbolKind(ib.program->vars[static_cast<std::size_t>(use.index)].kind);
    case NameUseKind::Const:
        return SymbolKind::Constant;
    case NameUseKind::Struct:
        return SymbolKind::StructType;
    case NameUseKind::Enum:
        return SymbolKind::EnumType;
    case NameUseKind::EnumMember:
        return SymbolKind::EnumMember;
    case NameUseKind::Field:
        return SymbolKind::Field;
    case NameUseKind::Module:
        return SymbolKind::Module;
    case NameUseKind::Macro:
        return SymbolKind::Macro;
    case NameUseKind::MacroParam:
        return SymbolKind::Parameter;
    }
    return SymbolKind::Local;
}

std::int32_t DefinitionFor(IndexBuilder& ib, const NameUse& use) {
    EntityKey key = KeyOf(use);
    auto known = ib.defs.find(key);
    if (known != ib.defs.end()) {
        return known->second;
    }
    Definition def = Describe(ib, use);
    def.has_loc = true;
    auto declared = ib.declared.find(key);
    if (declared != ib.declared.end()) {
        def.loc = declared->second;
    }
    def.library = InLib(ib, def.loc.file);
    std::int32_t id = AddDefinition(ib, def);
    ib.defs[key] = id;
    return id;
}

std::string HeaderComment(const SourceFile& file) {
    std::string doc;
    std::string text;
    for (std::size_t line = 0; line < file.line_starts.size(); line++) {
        if (!CommentLine(LineText(file, line), text)) {
            break;
        }
        doc += (line == 0 ? "" : "\n") + text;
    }
    return doc;
}

void AddDocs(IndexBuilder& ib) {
    EditorIndex& index = *ib.out;
    for (std::size_t t = 0; t < index.tokens.size(); t++) {
        const IndexToken& token = index.tokens[t];
        if (!token.declaration || token.def < 0) {
            continue;
        }
        Definition& def = index.definitions[static_cast<std::size_t>(token.def)];
        if (def.kind == SymbolKind::Module || !def.doc.empty()) {
            continue;
        }
        if (def.loc.file == token.loc.file && def.loc.span.start == token.loc.span.start) {
            def.doc = DocFor(index, def, t);
        }
    }
}

} // namespace tisple
