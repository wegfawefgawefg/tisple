#pragma once

// the scripted input of `tisple machine` and `tisple run`: --keys, --mouse and --joystick, and
// the numbers
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
// "cycle:x,y,buttons;...": from that cycle paddles 0 and 1 read x and y (0-255), buttons a
// number whose bits 0-2 hold buttons 0-2 down (1 open-apple, 2 closed-apple, 3 both)
bool ScheduleJoystickInput(Apple2& a, const char* spec, std::string& error);
// --model: iie-enhanced, iie, iic, iiplus, iigs
bool ParseModel(const char* name, MachineModel& out);

} // namespace tisple
