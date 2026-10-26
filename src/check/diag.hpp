#pragma once

#include "reader/reader.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

// where a form came from: file, byte span, and the macro call that made it (0: none)
struct Loc {
    std::uint32_t file = 0;
    Span span;
    std::uint32_t expansion = 0;
};

// one macro call: the expanded forms it made point back at it
struct Expansion {
    std::string macro;
    Loc call;
    std::int32_t module = -1; // where the macro is defined: its template's free names live there
};

// every file of a program plus the macro calls, so any Loc can be printed
struct Sources {
    std::vector<SourceFile> files;
    std::vector<Expansion> expansions;
};

struct Diagnostic {
    Loc loc;
    std::string message;
};

Loc LocOf(std::uint32_t file, const Node& node);

// "path:line:col: message", plus " (in macro swap)" for forms a macro made
std::string FormatLoc(const Sources& sources, const Loc& loc);
std::string FormatDiagnostic(const Sources& sources, const Diagnostic& diagnostic);
std::string FormatDiagnostics(const Sources& sources, const std::vector<Diagnostic>& diagnostics);

void AddError(std::vector<Diagnostic>& errors, const Loc& loc, const std::string& message);

} // namespace tisple
