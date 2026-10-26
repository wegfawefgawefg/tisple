#pragma once

#include "source_file.hpp"

#include <string>

namespace tisple {

// the file with line numbers, colored by the real lexer when color is on;
// returns false when the file does not read (the error is marked in out)
bool RenderShow(const SourceFile& file, bool color, std::string& out);

} // namespace tisple
