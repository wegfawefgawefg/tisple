#pragma once

// the window front end. declared here so the command can call it; defined in sdl_window.cpp,
// which is only built when SDL3 is (cmake option TISPLE_SDL).

#include "machine/apple2.hpp"
#include "machine/taps.hpp"

#include <string>
#include <vector>

namespace tisple {

struct WindowOptions {
    int scale = 2;                      // window size in multiples of 560 x 384
    VideoLook look = VideoLook::kColor; // F2 cycles color, ntsc, mono
    uint64_t max_cycles = 0;            // 0: run until the window closes
    std::string title = "Tisple ][";
    std::vector<Tap> taps; // --taps: clicks press these keys instead of moving the mouse card
};

// returns the process exit code; closing the window returns at once
using MachineWindowFn = int (*)(Apple2& a, const WindowOptions& options);

int RunMachineWindow(Apple2& a, const WindowOptions& options);

} // namespace tisple
