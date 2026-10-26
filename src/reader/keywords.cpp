#include "reader/keywords.hpp"

namespace tisple {

namespace {

// keep in step with docs/language.md; editors/ is generated from these
const char* const kSpecialForms[] = {
    "fn",       "let",
    "const",    "struct",
    "enum",     "if",
    "do",       "while",
    "for",      "match",
    "return",   "break",
    "continue", "args",
    "set",      "and",
    "or",       ".",
    "_",        "as",
    "macro",    "import",
    "quote",    "quasiquote",
    "unquote",  "unquote_splicing",
};

const char* const kTypeNames[] = {"u8", "i8", "u16", "i16", "bool", "ptr", "array"};

const char* const kBuiltins[] = {
    "at", "load", "store", "addr", "io_read", "io_write", "not", "+", "-", "*", "/",  "%",
    "=",  "!=",   "<",     "<=",   ">",       ">=",       "&",   "|", "^", "~", "<<", ">>",
};

const char* const kConstantNames[] = {"true", "false"};

bool InList(std::span<const char* const> list, std::string_view text) {
    for (const char* entry : list) {
        if (text == entry) {
            return true;
        }
    }
    return false;
}

bool IsUpper(char c) {
    return c >= 'A' && c <= 'Z';
}

bool IsLower(char c) {
    return c >= 'a' && c <= 'z';
}

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

// last segment of a qualified name: hgr.plot -> plot, gfx/Sprite -> Sprite
std::string_view LastSegment(std::string_view text) {
    std::size_t cut = text.find_last_of("./");
    if (cut != std::string_view::npos && cut + 1 < text.size()) {
        text = text.substr(cut + 1);
    }
    if (!text.empty() && text.back() == '#') {
        text = text.substr(0, text.size() - 1);
    }
    return text;
}

} // namespace

std::span<const char* const> SpecialForms() {
    return kSpecialForms;
}

std::span<const char* const> TypeNames() {
    return kTypeNames;
}

std::span<const char* const> Builtins() {
    return kBuiltins;
}

std::span<const char* const> ConstantNames() {
    return kConstantNames;
}

bool IsPascalCase(std::string_view name) {
    if (name.empty() || !IsUpper(name[0])) {
        return false;
    }
    bool has_lower = false;
    for (char c : name) {
        if (!IsUpper(c) && !IsLower(c) && !IsDigit(c)) {
            return false;
        }
        has_lower = has_lower || IsLower(c);
    }
    return has_lower;
}

bool IsAllCaps(std::string_view name) {
    if (name.empty() || !IsUpper(name[0])) {
        return false;
    }
    for (char c : name) {
        if (!IsUpper(c) && !IsDigit(c) && c != '_') {
            return false;
        }
    }
    return true;
}

SymbolClass ClassifySymbol(std::string_view text) {
    if (InList(kSpecialForms, text)) {
        return SymbolClass::SpecialForm;
    }
    if (InList(kTypeNames, text)) {
        return SymbolClass::Type;
    }
    if (InList(kBuiltins, text)) {
        return SymbolClass::Builtin;
    }
    if (InList(kConstantNames, text)) {
        return SymbolClass::Constant;
    }
    std::string_view last = LastSegment(text);
    if (IsPascalCase(last)) {
        return SymbolClass::Type;
    }
    if (IsAllCaps(last)) {
        return SymbolClass::Constant;
    }
    return SymbolClass::Name;
}

} // namespace tisple
