#include "ide/index_internal.hpp"

namespace tisple {

namespace {

// special forms, built-ins and primitive types: usage lines for hover, from docs/language.md

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

} // namespace

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

} // namespace tisple
