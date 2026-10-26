#pragma once

// Rust naming: PascalCase types, SCREAMING_SNAKE_CASE constants, snake_case the rest

#include <string>
#include <string_view>

namespace tisple {

bool IsSnakeName(std::string_view name);
bool IsTypeName(std::string_view name);
bool IsConstantName(std::string_view name);
// tmp__3: what tmp# becomes; user names never contain "__"
bool IsFreshName(std::string_view name);

std::string ToSnakeName(std::string_view name);
std::string ToTypeName(std::string_view name);
std::string ToConstantName(std::string_view name);

// special forms, types, operators and true/false: none can name anything
bool IsBuiltinName(std::string_view name);

} // namespace tisple
