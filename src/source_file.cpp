#include "source_file.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace tisple {

SourceFile SourceFileFromText(const std::string& path, const std::string& text) {
    SourceFile file;
    file.path = path;
    file.text = text;
    file.line_starts.push_back(0);
    for (std::uint32_t i = 0; i < file.text.size(); i++) {
        if (file.text[i] == '\n') {
            file.line_starts.push_back(i + 1);
        }
    }
    return file;
}

bool LoadSourceFile(const std::string& path, SourceFile& out, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    out = SourceFileFromText(path, contents.str());
    return true;
}

SourcePos PosFromOffset(const SourceFile& file, std::uint32_t offset) {
    // last line start at or before offset
    auto it = std::upper_bound(file.line_starts.begin(), file.line_starts.end(), offset);
    std::uint32_t line_index = static_cast<std::uint32_t>(it - file.line_starts.begin()) - 1;
    SourcePos pos;
    pos.line = line_index + 1;
    pos.column = offset - file.line_starts[line_index] + 1;
    return pos;
}

} // namespace tisple
