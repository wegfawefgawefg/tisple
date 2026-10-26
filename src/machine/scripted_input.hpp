#pragma once

// the scripted input of `tisple machine` and `tisple run`: --keys and --mouse, and the numbers
// they are written in

#include "machine/apple2.hpp"

#include <cstdint>
#include <string>

namespace tisple {

// $hex, 0xhex or decimal
bool ParseNumber(const char* text, uint64_t& out);
// a number of cycles, or of frames with an f suffix (30f)
bool ParseCycle(const std::string& text, uint64_t& out);

// "cycle:key,...": a character, $hex, or a name (RETURN ESC LEFT ...)
bool ScheduleKeys(Apple2& a, const char* spec, std::string& error);
// "cycle:x,y,button;...": from that cycle the mouse card's mouse is at x, y, button 0 or 1
bool ScheduleMouseInput(Apple2& a, const char* spec, std::string& error);

} // namespace tisple
