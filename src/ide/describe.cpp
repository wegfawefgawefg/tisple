#include "ide/index_internal.hpp"

#include <cstdio>

namespace tisple {

namespace {

// ---- built-ins: usage lines for hover, from docs/language.md

struct BuiltinInfo {
    const char* name;
    SymbolKind kind;
    const char* usage;
    const char* doc;
};

const BuiltinInfo kBuiltinInfo[] = {
    {"fn", SymbolKind::SpecialForm, "(fn name ((x u8) ...) type body ...)",
     "a function; () when it takes nothing, void when it returns nothing"},
    {"let", SymbolKind::SpecialForm, "(let name type value)",
     "a variable; a top-level let may leave out the value"},
    {"const", SymbolKind::SpecialForm, "(const NAME type value)",
     "a compile-time constant, a scalar or a table of scalars"},
    {"struct", SymbolKind::SpecialForm, "(struct Name ((x u8) (y u8)))", "a struct type"},
    {"enum", SymbolKind::SpecialForm, "(enum Name (a b (c 7)))", "an enum, stored as u8"},
    {"if", SymbolKind::SpecialForm, "(if cond then else)", "both arms are one form"},
    {"do", SymbolKind::SpecialForm, "(do body ...)", "groups forms; its value is the last"},
    {"while", SymbolKind::SpecialForm, "(while cond body ...)", "loops while cond is true"},
    {"for", SymbolKind::SpecialForm, "(for i type start end body ...)",
     "i from start up to, not including, end"},
    {"match", SymbolKind::SpecialForm, "(match x (pattern body ...) ...)",
     "the first clause whose pattern matches"},
    {"return", SymbolKind::SpecialForm, "(return value) or (return)", "leaves the function"},
    {"break", SymbolKind::SpecialForm, "(break)", "leaves the innermost loop"},
    {"continue", SymbolKind::SpecialForm, "(continue)", "starts the next round of the loop"},
    {"set", SymbolKind::SpecialForm, "(set place value)", "assigns to a name, s.x or (at ...)"},
    {"and", SymbolKind::SpecialForm, "(and a b)", "short-circuit, on bool"},
    {"or", SymbolKind::SpecialForm, "(or a b)", "short-circuit, on bool"},
    {".", SymbolKind::SpecialForm, "(. base x ...)", "a field, written only by macro templates"},
    {"_", SymbolKind::SpecialForm, "(_ body ...)", "the match clause for every other value"},
    {"as", SymbolKind::SpecialForm, "(as type x)", "the only conversion"},
    {"make", SymbolKind::SpecialForm, "(make Type :x 1 ...) or (make (array t n) v ...)",
     "builds a struct or array value"},
    {"macro", SymbolKind::SpecialForm, "(macro name (params) `template)", "a template macro"},
    {"import", SymbolKind::SpecialForm, "(import path prefix)",
     "a module, from the project folder or lib/"},
    {"quote", SymbolKind::SpecialForm, "'form", "the form itself, unevaluated"},
    {"quasiquote", SymbolKind::SpecialForm, "`form", "a macro template"},
    {"unquote", SymbolKind::SpecialForm, ",name", "a macro argument dropped in"},
    {"unquote_splicing", SymbolKind::SpecialForm, ",@name", "a macro argument's forms spliced"},
    {"at", SymbolKind::Builtin, "(at arr i field ...)", "an array element, or a field of it"},
    {"load", SymbolKind::Builtin, "(load p field ...)", "reads through a pointer"},
    {"store", SymbolKind::Builtin, "(store p field ... v)", "writes through a pointer"},
    {"addr_of", SymbolKind::Builtin, "(addr_of place)",
     "the address of a variable, element or field"},
    {"io_read", SymbolKind::Builtin, "(io_read u8 addr)",
     "hardware read; never dropped, merged or reordered"},
    {"io_write", SymbolKind::Builtin, "(io_write u8 addr v)",
     "hardware write; never dropped, merged or reordered"},
    {"not", SymbolKind::Builtin, "(not b)", "bool negation"},
    {"+", SymbolKind::Builtin, "(+ a b)", "add; wraps; a pointer moves by elements"},
    {"-", SymbolKind::Builtin, "(- a b)", "subtract; wraps"},
    {"*", SymbolKind::Builtin, "(* a b)", "multiply; wraps"},
    {"/", SymbolKind::Builtin, "(/ a b)", "divide, truncating toward zero"},
    {"%", SymbolKind::Builtin, "(% a b)", "remainder, with the dividend's sign"},
    {"=", SymbolKind::Builtin, "(= a b)", "equal"},
    {"!=", SymbolKind::Builtin, "(!= a b)", "not equal"},
    {"<", SymbolKind::Builtin, "(< a b)", "less than; signed for i8 and i16"},
    {"<=", SymbolKind::Builtin, "(<= a b)", "at most; signed for i8 and i16"},
    {">", SymbolKind::Builtin, "(> a b)", "greater than; signed for i8 and i16"},
    {">=", SymbolKind::Builtin, "(>= a b)", "at least; signed for i8 and i16"},
    {"&", SymbolKind::Builtin, "(& a b)", "bitwise and"},
    {"|", SymbolKind::Builtin, "(| a b)", "bitwise or"},
    {"^", SymbolKind::Builtin, "(^ a b)", "bitwise xor"},
    {"~", SymbolKind::Builtin, "(~ a)", "bitwise not"},
    {"<<", SymbolKind::Builtin, "(<< x n)", "shift left; n is u8"},
    {">>", SymbolKind::Builtin, "(>> x n)", "shift right, arithmetic for signed; n is u8"},
    {"u8", SymbolKind::PrimitiveType, "u8", "8-bit unsigned, 0 to 255"},
    {"i8", SymbolKind::PrimitiveType, "i8", "8-bit signed, -128 to 127"},
    {"u16", SymbolKind::PrimitiveType, "u16", "16-bit unsigned, 0 to 65535, low byte first"},
    {"i16", SymbolKind::PrimitiveType, "i16", "16-bit signed, -32768 to 32767"},
    {"bool", SymbolKind::PrimitiveType, "bool", "true or false; not an integer"},
    {"void", SymbolKind::PrimitiveType, "void", "the return type of a fn that returns nothing"},
    {"ptr", SymbolKind::PrimitiveType, "(ptr t)", "the 16-bit address of a t"},
    {"array", SymbolKind::PrimitiveType, "(array t n)", "n elements of t; n a constant"},
};

const BuiltinInfo* FindBuiltin(const std::string& text) {
    for (const BuiltinInfo& info : kBuiltinInfo) {
        if (text == info.name) {
            return &info;
        }
    }
    return nullptr;
}

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
        for (const std::string& param : info.params) {
            params += (params.empty() ? "" : " ") + param;
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

bool BuiltinKind(const std::string& text, SymbolKind& kind) {
    const BuiltinInfo* info = FindBuiltin(text);
    if (!info) {
        return false;
    }
    kind = info->kind;
    return true;
}

std::int32_t BuiltinDefinition(IndexBuilder& ib, const std::string& text, SymbolKind kind) {
    auto known = ib.builtins.find(text);
    if (known != ib.builtins.end()) {
        return known->second;
    }
    const BuiltinInfo* info = FindBuiltin(text);
    Definition def;
    def.name = text;
    def.qualified = text;
    def.kind = kind;
    def.type = info ? info->usage : text;
    def.doc = info ? info->doc : "";
    def.library = true;
    std::int32_t id = AddDefinition(ib, def);
    ib.builtins[text] = id;
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
