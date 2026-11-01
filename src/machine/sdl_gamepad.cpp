// the window's game controller as the Apple's joystick: the first controller SDL opens drives
// the game port. The left stick's x and y are paddles 0 and 1 (0 left or up, 255 right or
// down, as a centred Apple joystick reads 127 or so); the south face button (A on an Xbox
// pad) is button 0, open-apple; east (B) is button 1, closed-apple; west and north (X, Y)
// are button 2. Host keys still go to the keyboard, alt keys still to buttons 0 and 1.

#include "machine/sdl_gamepad.hpp"

namespace tisple {

namespace {

SDL_Gamepad* g_pad = nullptr; // the one that drives the port

uint8_t AxisToPaddle(int16_t value) {
    int v = (static_cast<int>(value) + 32768) * 255 / 65535;
    return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}

int ButtonFor(uint8_t button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return 0;
    case SDL_GAMEPAD_BUTTON_EAST:
        return 1;
    case SDL_GAMEPAD_BUTTON_WEST:
    case SDL_GAMEPAD_BUTTON_NORTH:
        return 2;
    default:
        return -1;
    }
}

bool IsOurs(SDL_JoystickID id) {
    return g_pad != nullptr && SDL_GetGamepadID(g_pad) == id;
}

} // namespace

bool HandleGamepadEvent(Apple2& a, const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (g_pad == nullptr) {
            g_pad = SDL_OpenGamepad(e.gdevice.which);
        }
        return true;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (IsOurs(e.gdevice.which)) {
            SDL_CloseGamepad(g_pad);
            g_pad = nullptr;
            SetPaddle(a, 0, kPaddleCentre);
            SetPaddle(a, 1, kPaddleCentre);
        }
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (!IsOurs(e.gaxis.which)) {
            return true;
        }
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            SetPaddle(a, 0, AxisToPaddle(e.gaxis.value));
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            SetPaddle(a, 1, AxisToPaddle(e.gaxis.value));
        }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int button = ButtonFor(e.gbutton.button);
        if (IsOurs(e.gbutton.which) && button >= 0) {
            SetButton(a, button, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace tisple
