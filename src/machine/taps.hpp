#pragma once

// `tisple machine --window --taps game.json`: clicks on the screen press keys, from the same
// "taps" a game gives the phone player (web/, docs/phone.md): boxes in hi-res pixels (280 x
// 192), each with the key it presses while the button is down.

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

struct Tap {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    uint8_t code = 0; // the key, as PressKey takes it
};

// the "taps" array of a game's player config; false with error when it can't be read
bool LoadTaps(const std::string& path, std::vector<Tap>& taps, std::string& error);

// the first tap whose box holds hi-res pixel (x, y), or nullptr
const Tap* TapAt(const std::vector<Tap>& taps, int x, int y);

} // namespace tisple
