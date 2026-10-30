#include "race/race_c.hpp"
#include "race/race_c_internal.hpp"

#include <cstdio>
#include <set>

// the whole C file: names, structs, globals with their initial values, prototypes, functions

namespace tisple {

namespace {

using racec::Writer;

const char* const kCKeywords[] = {
    "auto",   "break",    "case",   "char",   "const",    "continue", "default", "do",
    "double", "else",     "enum",   "extern", "float",    "for",      "goto",    "if",
    "inline", "int",      "long",   "main",   "register", "restrict", "return",  "short",
    "signed", "sizeof",   "static", "struct", "switch",   "typedef",  "union",   "unsigned",
    "void",   "volatile", "while",  "asm",    "fastcall", "cdecl",    "far",     "near"};

// a Tisple name as a C identifier: the part after the module, other characters as _
std::string Identifier(const std::string& name) {
    std::string text = name.substr(name.rfind('.') + 1);
    for (char& c : text) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!ok) {
            c = '_';
        }
    }
    for (const char* keyword : kCKeywords) {
        if (text == keyword) {
            return text + "_";
        }
    }
    return text;
}

void NameEverything(Writer& w) {
    const Program& program = *w.program;
    w.var_names.assign(program.vars.size(), "");
    std::vector<std::set<std::string>> taken(program.functions.size());
    for (std::size_t i = 0; i < program.vars.size(); i++) {
        const Var& var = program.vars[i];
        if (var.kind == VarKind::Global || var.kind == VarKind::Const) {
            w.var_names[i] = "t_" + Identifier(var.name);
            continue;
        }
        // locals by their own names, but two of one name in a function are told apart
        std::string name = Identifier(var.name);
        if (name.rfind("tmp", 0) == 0 || name.rfind("brk", 0) == 0) {
            name = "v_" + name;
        }
        if (var.function >= 0) {
            std::set<std::string>& seen = taken[static_cast<std::size_t>(var.function)];
            if (!seen.insert(name).second) {
                name += "_" + std::to_string(i);
            }
        }
        w.var_names[i] = name;
    }
    w.fn_names.assign(program.functions.size(), "");
    for (std::size_t i = 0; i < program.functions.size(); i++) {
        bool is_main = static_cast<std::int32_t>(i) == program.main;
        w.fn_names[i] = is_main ? "main" : "t_" + Identifier(program.functions[i].name);
    }
}

// "uint8_t name[4][8]" for arrays, as Declare for anything else
std::string DeclareAny(const Writer& w, TypeId type, const std::string& name) {
    std::string dims;
    while (TypeOf(*w.program, type).kind == TypeKind::Array) {
        dims += "[" + std::to_string(TypeOf(*w.program, type).count) + "]";
        type = TypeOf(*w.program, type).elem;
    }
    return w.Declare(type, name) + dims;
}

std::string Initializer(const Writer& w, TypeId type, const std::vector<std::uint8_t>& bytes,
                        std::uint32_t offset) {
    const Type& t = TypeOf(*w.program, type);
    if (IsScalarKind(t.kind)) {
        std::uint32_t raw = bytes[offset];
        if (t.size == 2) {
            raw |= static_cast<std::uint32_t>(bytes[offset + 1]) << 8;
        }
        return w.Literal(type, raw);
    }
    std::string text = "{";
    if (t.kind == TypeKind::Struct) {
        const StructDecl& sd = w.program->structs[static_cast<std::size_t>(t.decl)];
        for (std::size_t i = 0; i < sd.fields.size(); i++) {
            text += (i == 0 ? "" : ", ") +
                    Initializer(w, sd.fields[i].type, bytes, offset + sd.fields[i].offset);
        }
        return text + "}";
    }
    std::uint32_t size = TypeOf(*w.program, t.elem).size;
    for (std::uint32_t i = 0; i < t.count; i++) {
        text += (i == 0 ? "" : ", ") + Initializer(w, t.elem, bytes, offset + i * size);
    }
    return text + "}";
}

std::string Structs(const Writer& w) {
    std::string text;
    for (const StructDecl& sd : w.program->structs) {
        text += "struct " + sd.name + " {\n";
        for (const Field& field : sd.fields) {
            text += "    " + DeclareAny(w, field.type, field.name) + ";\n";
        }
        text += "};\n";
    }
    return text;
}

bool Globals(Writer& w, std::string& text) {
    const Program& program = *w.program;
    for (std::size_t i = 0; i < program.vars.size(); i++) {
        const Var& var = program.vars[i];
        if (var.kind != VarKind::Global && var.kind != VarKind::Const) {
            continue;
        }
        const std::string& name = w.var_names[i];
        if (var.placement.has_address) {
            if (!IsScalarKind(KindOf(program, var.type)) || !var.init.empty()) {
                return w.Fail(var.name + ": only a scalar without a value can have :address");
            }
            char line[160];
            std::snprintf(line, sizeof(line), "#define %s RACE_FIXED(%s, 0x%04X)\n", name.c_str(),
                          w.CType(var.type).c_str(), var.placement.address);
            text += line;
            continue;
        }
        text += std::string("static ") + (var.kind == VarKind::Const ? "const " : "") +
                DeclareAny(w, var.type, name);
        if (!var.init.empty()) {
            text += " = " + Initializer(w, var.type, var.init, 0);
        }
        text += ";\n";
    }
    return true;
}

} // namespace

bool WriteRaceC(const Program& program, std::string& out, std::string& error) {
    Writer w;
    w.program = &program;
    NameEverything(w);
    out = "#include \"random.h\"\n\n" + Structs(w) + "\n";
    if (!Globals(w, out)) {
        error = w.error;
        return false;
    }
    out += "\n";
    for (std::size_t i = 0; i < program.functions.size(); i++) {
        std::int32_t index = static_cast<std::int32_t>(i);
        if (index != program.main) {
            out += w.Prototype(index, w.Params(index)) + ";\n";
        }
    }
    for (std::size_t i = 0; i < program.functions.size(); i++) {
        out += "\n" + w.Function(static_cast<std::int32_t>(i));
    }
    error = w.error;
    return error.empty();
}

} // namespace tisple
