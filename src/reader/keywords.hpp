#pragma once

#include <span>
#include <string_view>

namespace tisple {

// what a symbol is, for coloring; the viewer and editor grammars share these lists
enum class SymbolClass : unsigned char {
    Name,
    SpecialForm,
    Type,
    Builtin,
    Constant,
};

std::span<const char* const> SpecialForms();
std::span<const char* const> TypeNames();
std::span<const char* const> Builtins();
std::span<const char* const> ConstantNames();

// exact list match first, then by spelling: PascalCase is a type, ALL_CAPS a constant
SymbolClass ClassifySymbol(std::string_view text);

bool IsPascalCase(std::string_view name);
bool IsAllCaps(std::string_view name);

} // namespace tisple
