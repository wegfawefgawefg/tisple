#include "check/names.hpp"

#include "reader/keywords.hpp"

namespace tisple {

namespace {

bool IsLower(char c) {
    return c >= 'a' && c <= 'z';
}

bool IsUpper(char c) {
    return c >= 'A' && c <= 'Z';
}

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

char Lower(char c) {
    return IsUpper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

char Upper(char c) {
    return IsLower(c) ? static_cast<char>(c - 'a' + 'A') : c;
}

bool InList(std::span<const char* const> list, std::string_view name) {
    for (const char* entry : list) {
        if (name == entry) {
            return true;
        }
    }
    return false;
}

// words of any spelling: readKey, read-key, READ_KEY and ReadKey all give read, key
std::string Words(std::string_view name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); i++) {
        char c = name[i];
        bool letter_or_digit = IsLower(c) || IsUpper(c) || IsDigit(c);
        if (!letter_or_digit) {
            if (!out.empty() && out.back() != ' ') {
                out += ' ';
            }
            continue;
        }
        bool boundary = IsUpper(c) && i > 0 && (IsLower(name[i - 1]) || IsDigit(name[i - 1]));
        if (boundary && !out.empty() && out.back() != ' ') {
            out += ' ';
        }
        out += Lower(c);
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

bool CleanUnderscores(std::string_view name) {
    return name.find("__") == std::string_view::npos && !name.empty() && name.back() != '_';
}

} // namespace

bool IsSnakeName(std::string_view name) {
    if (name.empty() || !IsLower(name[0]) || !CleanUnderscores(name)) {
        return false;
    }
    for (char c : name) {
        if (!IsLower(c) && !IsDigit(c) && c != '_') {
            return false;
        }
    }
    return true;
}

bool IsTypeName(std::string_view name) {
    if (name.empty() || !IsUpper(name[0])) {
        return false;
    }
    for (char c : name) {
        if (!IsUpper(c) && !IsLower(c) && !IsDigit(c)) {
            return false;
        }
    }
    return true;
}

bool IsConstantName(std::string_view name) {
    return IsAllCaps(name) && CleanUnderscores(name);
}

bool IsFreshName(std::string_view name) {
    std::size_t cut = name.rfind("__");
    if (cut == std::string_view::npos || cut == 0 || cut + 2 >= name.size()) {
        return false;
    }
    for (std::size_t i = cut + 2; i < name.size(); i++) {
        if (!IsDigit(name[i])) {
            return false;
        }
    }
    return true;
}

std::string ToSnakeName(std::string_view name) {
    std::string words = Words(name);
    for (char& c : words) {
        c = c == ' ' ? '_' : c;
    }
    if (words.empty() || !IsLower(words[0])) {
        words = "x_" + words;
    }
    return words;
}

std::string ToTypeName(std::string_view name) {
    std::string words = Words(name);
    std::string out;
    bool start = true;
    for (char c : words) {
        if (c == ' ') {
            start = true;
            continue;
        }
        out += start ? Upper(c) : c;
        start = false;
    }
    if (out.empty() || !IsUpper(out[0])) {
        out = "T" + out;
    }
    return out;
}

std::string ToConstantName(std::string_view name) {
    std::string words = Words(name);
    for (char& c : words) {
        c = c == ' ' ? '_' : Upper(c);
    }
    if (words.empty() || !IsUpper(words[0])) {
        words = "K_" + words;
    }
    return words;
}

bool IsBuiltinName(std::string_view name) {
    return InList(SpecialForms(), name) || InList(TypeNames(), name) || InList(Builtins(), name) ||
           InList(ConstantNames(), name);
}

bool IsReservedValueName(std::string_view name) {
    return InList(TypeNames(), name) || InList(ConstantNames(), name) || name == "_" || name == ".";
}

} // namespace tisple
