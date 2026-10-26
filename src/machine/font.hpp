#pragma once

#include <cstdint>

namespace tisple {

enum class TextStyle : uint8_t {
    kNormal,
    kInverse,
    kFlash,
};

struct TextCell {
    uint8_t ascii = 0x20;
    TextStyle style = TextStyle::kNormal;
};

// dots of one glyph row, bit 0 leftmost, 7 dots wide
uint8_t GlyphRow(uint8_t ascii, int row);

// a screen byte as the iie character generator reads it
TextCell DecodeTextByte(uint8_t code, bool altchar);

} // namespace tisple
