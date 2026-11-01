#pragma once

// the window's game controller as the Apple's joystick (sdl_gamepad.cpp): the left stick is
// paddles 0 and 1, the face buttons the three buttons. built only with TISPLE_SDL.

#include "machine/apple2.hpp"

#include <SDL3/SDL.h>

namespace tisple {

// a controller added, removed, moved or pressed; false for any other event
bool HandleGamepadEvent(Apple2& a, const SDL_Event& e);

} // namespace tisple
