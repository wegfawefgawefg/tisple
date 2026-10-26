#include "check/diag.hpp"

namespace tisple {

Loc LocOf(std::uint32_t file, const Node& node) {
    Loc loc;
    loc.file = file;
    loc.span = node.span;
    loc.expansion = node.expansion;
    return loc;
}

std::string FormatLoc(const Sources& sources, const Loc& loc) {
    if (loc.file >= sources.files.size()) {
        return "tisple";
    }
    const SourceFile& file = sources.files[loc.file];
    SourcePos pos = PosFromOffset(file, loc.span.start);
    return file.path + ":" + std::to_string(pos.line) + ":" + std::to_string(pos.column);
}

std::string FormatDiagnostic(const Sources& sources, const Diagnostic& diagnostic) {
    std::string out = FormatLoc(sources, diagnostic.loc) + ": " + diagnostic.message;
    // the span points at the outermost call the user wrote; name every macro on the way
    std::uint32_t expansion = diagnostic.loc.expansion;
    std::string chain;
    while (expansion != 0 && expansion <= sources.expansions.size()) {
        const Expansion& entry = sources.expansions[expansion - 1];
        chain += chain.empty() ? " (in macro " : " from macro ";
        chain += entry.macro;
        expansion = entry.call.expansion;
    }
    if (!chain.empty()) {
        out += chain + ")";
    }
    return out;
}

std::string FormatDiagnostics(const Sources& sources, const std::vector<Diagnostic>& diagnostics) {
    std::string out;
    for (const Diagnostic& diagnostic : diagnostics) {
        out += FormatDiagnostic(sources, diagnostic) + "\n";
    }
    return out;
}

void AddError(std::vector<Diagnostic>& errors, const Loc& loc, const std::string& message) {
    errors.push_back(Diagnostic{loc, message});
}

} // namespace tisple
