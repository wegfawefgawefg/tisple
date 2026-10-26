#pragma once

#include "reader/reader.hpp"

#include <string>

namespace tisple {

constexpr std::uint32_t kFormatColumns = 100;
constexpr std::uint32_t kFormatIndent = 4;

// canonical text for a tree; with keep_trivia false, comments and blank lines are dropped
std::string FormatTree(const SourceFile& file, const Tree& tree, bool keep_trivia);

// read then format; false with error filled when the file does not read
bool FormatSource(const SourceFile& file, std::string& out, ReadError& error);

} // namespace tisple
