#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

// one loaded .tsp file; byte offsets index into text
struct SourceFile {
    std::string path;
    std::string text;
    std::vector<std::uint32_t> line_starts;
};

struct SourcePos {
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

bool LoadSourceFile(const std::string& path, SourceFile& out, std::string& error);
SourceFile SourceFileFromText(const std::string& path, const std::string& text);
SourcePos PosFromOffset(const SourceFile& file, std::uint32_t offset);

} // namespace tisple
