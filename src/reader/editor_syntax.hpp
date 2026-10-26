#pragma once

#include <string>

namespace tisple {

// editor grammars generated from the keyword lists in keywords.cpp, so editors
// and `tisple show` agree; `tisple gen-editors editors` writes them
std::string VsCodeGrammar();
std::string NvimSyntax();

constexpr const char* kVsCodeGrammarPath = "vscode/syntaxes/tisple.tmLanguage.json";
constexpr const char* kNvimSyntaxPath = "nvim/syntax/tisple.vim";

} // namespace tisple
