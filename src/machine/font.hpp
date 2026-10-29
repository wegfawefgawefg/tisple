#pragma once

#include <cstdint>

namespace tisple {

enum class TextStyle : uint8_t {
    kNormal,
    kInverse,
    kFlash,
};

struct TextCell {
    uint8_t ascii = 0x20; // for MouseText, the glyph's number 0-31
    TextStyle style = TextStyle::kNormal;
    bool mousetext = false;
};

// dots of one glyph row, bit 0 leftmost, 7 dots wide
uint8_t GlyphRow(uint8_t ascii, int row);
// MouseText glyph 0-31 ($40-$5F in the alternate set)
uint8_t MouseTextRow(uint8_t index, int row);
// a decoded cell's row, either kind
uint8_t CellRow(const TextCell& cell, int row);

// a screen byte as the iie character generator reads it
TextCell DecodeTextByte(uint8_t code, bool altchar);

} // namespace tisple
