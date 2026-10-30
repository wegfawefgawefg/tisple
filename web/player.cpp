// The phone player's machine: tisple machine compiled to WebAssembly (web/build.sh), driven
// by player.js one slice at a time. It boots a disk with our own roms, renders the screen to
// RGBA for a canvas and the speaker and Mockingboard to 16-bit samples for WebAudio. While
// the disk motor is on, player.js runs it flat out, so a disk boots in a moment.

#include "disk/image.hpp"
#include "disk/machine_disks.hpp"
#include "machine/apple2.hpp"
#include "machine/disk2.hpp"
#include "machine/mockingboard.hpp"
#include "machine/mockingboard_audio.hpp"
#include "machine/speaker_audio.hpp"

#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace tisple;

struct Player {
    std::unique_ptr<Apple2> a;
    Framebuffer fb;
    std::vector<uint8_t> rgba = std::vector<uint8_t>(kScreenWidth * kScreenHeight * 4);
    SpeakerRenderer speaker;
    MockingboardRenderer chips;
    std::vector<int16_t> samples;
    std::string error;
};

Player g_player;

void StartAudio(Player& p) {
    p.speaker = SpeakerRenderer();
    p.speaker.next_toggle = p.a->speaker.toggle_count;
    p.speaker.samples_done = SamplesForCycles(Cycles(*p.a));
    p.chips = MockingboardRenderer();
    StartMockingboardAudio(p.chips, *p.a);
}

} // namespace

extern "C" {

// a fresh machine with the disk in drive 1 and a Mockingboard in slot (0: none), booting
EMSCRIPTEN_KEEPALIVE int tp_boot(const uint8_t* disk, int size, int mockingboard_slot) {
    Player& p = g_player;
    p.a = NewApple2();
    MachineDisks disks;
    disks.images[0].assign(disk, disk + size);
    disks.formats[0] = DiskFormat::kDos;
    if (!SetUpDisks(*p.a, disks, p.error)) {
        std::fprintf(stderr, "player: %s\n", p.error.c_str());
        return 0;
    }
    if (mockingboard_slot != 0) {
        InsertMockingboard(*p.a, mockingboard_slot);
    }
    StartDiskBoot(*p.a);
    StartAudio(p);
    return 1;
}

// runs the machine on; with sound, renders it into the sample buffer (tp_samples) and returns
// how many samples, else drops it. 0 once the program has stopped
EMSCRIPTEN_KEEPALIVE int tp_run(int cycles, int sound) {
    Player& p = g_player;
    if (!p.a || p.a->stopped) {
        return 0;
    }
    RunApple2(*p.a, static_cast<uint64_t>(cycles));
    p.samples.clear();
    if (!sound) {
        StartAudio(p);
        return 0;
    }
    RenderSpeaker(p.speaker, *p.a, Cycles(*p.a), p.samples);
    if (HasMockingboard(*p.a)) {
        MixMockingboard(p.chips, *p.a, Cycles(*p.a), p.samples, 0);
    }
    return static_cast<int>(p.samples.size());
}

EMSCRIPTEN_KEEPALIVE const int16_t* tp_samples() {
    return g_player.samples.data();
}

// the last whole frame, 560 x 192 RGBA; look: 0 color, 1 ntsc, 2 mono
EMSCRIPTEN_KEEPALIVE const uint8_t* tp_screen(int look) {
    Player& p = g_player;
    if (!p.a) {
        return p.rgba.data();
    }
    RenderFrame(p.a->video.done, static_cast<VideoLook>(look), p.fb);
    for (size_t i = 0; i < p.fb.pixels.size(); i++) {
        uint32_t c = p.fb.pixels[i];
        p.rgba[i * 4 + 0] = static_cast<uint8_t>(c >> 16);
        p.rgba[i * 4 + 1] = static_cast<uint8_t>(c >> 8);
        p.rgba[i * 4 + 2] = static_cast<uint8_t>(c);
        p.rgba[i * 4 + 3] = 255;
    }
    return p.rgba.data();
}

EMSCRIPTEN_KEEPALIVE void tp_key_down(int code) {
    if (g_player.a) {
        PressKey(*g_player.a, static_cast<uint8_t>(code));
    }
}

EMSCRIPTEN_KEEPALIVE void tp_key_up() {
    if (g_player.a) {
        ReleaseKey(*g_player.a);
    }
}

// open apple (0) and closed apple (1)
EMSCRIPTEN_KEEPALIVE void tp_button(int index, int down) {
    if (g_player.a) {
        SetButton(*g_player.a, index, down != 0);
    }
}

// the drive is spinning: player.js runs flat out and mutes
EMSCRIPTEN_KEEPALIVE int tp_disk_busy() {
    return g_player.a && g_player.a->slots.disk2.motor_on ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE double tp_cycles() {
    return g_player.a ? static_cast<double>(Cycles(*g_player.a)) : 0;
}

// a byte of memory as the program sees it, without side effects: for headless checks that
// play a game by its state (the page itself never reads the program's memory)
EMSCRIPTEN_KEEPALIVE int tp_peek(int addr) {
    return g_player.a ? PeekMemory(*g_player.a, static_cast<uint16_t>(addr)) : 0;
}

EMSCRIPTEN_KEEPALIVE int tp_stopped() {
    return g_player.a && g_player.a->stopped ? 1 : 0;
}

} // extern "C"
