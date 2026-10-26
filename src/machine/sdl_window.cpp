// `tisple machine --window`: the machine at real speed in an SDL3 window, with keyboard and
// speaker audio. built only with TISPLE_SDL. it never raises itself or grabs input; closing
// the window returns at once.

#include "machine/speaker_audio.hpp"
#include "machine/window.hpp"

#include <SDL3/SDL.h>
#include <cstdio>
#include <vector>

namespace tisple {

namespace {

constexpr uint64_t kNsPerSecond = 1000000000ull;
constexpr int kLatencyTargetMs = 40;         // audio queued ahead of the speaker
constexpr int kLatencyMaxMs = 120;           // past this, drop the queue and start again
constexpr int kLatencyMinMs = 10;            // below this, pad with the last level
constexpr uint64_t kTurboSliceNs = 14000000; // unthrottled: run this long per shown frame

struct WindowState {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_AudioStream* audio = nullptr;
    bool vsync = false;
    bool turbo = false;
    VideoLook look = VideoLook::kColor;
    bool reported_stop = false;
    Framebuffer fb;
    SpeakerRenderer speaker;
    std::vector<int16_t> samples;
    // real-time pacing: apple cycle base_cycle was due at host time base_ns
    uint64_t base_ns = 0;
    uint64_t base_cycle = 0;
};

uint32_t BytesForMs(int ms) {
    return static_cast<uint32_t>(kAudioRate * 2 * ms / 1000);
}

void PushSilence(WindowState& w, int ms) {
    int16_t level = w.samples.empty() ? 0 : w.samples.back();
    std::vector<int16_t> pad(static_cast<size_t>(kAudioRate * ms / 1000), level);
    SDL_PutAudioStreamData(w.audio, pad.data(), static_cast<int>(pad.size() * 2));
}

void SkipAudio(WindowState& w, const Apple2& a) {
    w.speaker.next_toggle = a.speaker.toggle_count;
    w.speaker.samples_done = SamplesForCycles(Cycles(a));
}

void FeedAudio(WindowState& w, const Apple2& a) {
    w.samples.clear();
    RenderSpeaker(w.speaker, a, Cycles(a), w.samples);
    if (w.audio == nullptr) {
        return;
    }
    if (!w.samples.empty()) {
        SDL_PutAudioStreamData(w.audio, w.samples.data(), static_cast<int>(w.samples.size() * 2));
    }
    int queued = SDL_GetAudioStreamQueued(w.audio);
    if (queued > static_cast<int>(BytesForMs(kLatencyMaxMs))) {
        SDL_ClearAudioStream(w.audio);
        PushSilence(w, kLatencyTargetMs);
    } else if (queued < static_cast<int>(BytesForMs(kLatencyMinMs))) {
        PushSilence(w, kLatencyTargetMs - kLatencyMinMs);
    }
}

void UpdateTitle(WindowState& w, const Apple2& a, const WindowOptions& options) {
    std::string title = options.title;
    if (w.turbo) {
        title += "  [unthrottled]";
    }
    if (w.look != VideoLook::kColor) {
        title += std::string("  [") + VideoLookName(w.look) + "]";
    }
    if (a.stopped) {
        title += "  [" + DescribeStop(a) + "]";
    }
    SDL_SetWindowTitle(w.window, title.c_str());
}

// SDL keys that are not plain text
bool SpecialKeyCode(SDL_Keycode key, uint8_t& code) {
    switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        code = 0x0D;
        return true;
    case SDLK_ESCAPE:
        code = 0x1B;
        return true;
    case SDLK_LEFT:
    case SDLK_BACKSPACE:
        code = 0x08;
        return true;
    case SDLK_RIGHT:
        code = 0x15;
        return true;
    case SDLK_UP:
        code = 0x0B;
        return true;
    case SDLK_DOWN:
        code = 0x0A;
        return true;
    case SDLK_TAB:
        code = 0x09;
        return true;
    case SDLK_DELETE:
        code = 0x7F;
        return true;
    default:
        return false;
    }
}

bool AnyKeyHeld() {
    int count = 0;
    const bool* state = SDL_GetKeyboardState(&count);
    for (int i = 0; i < count; i++) {
        if (state[i]) {
            return true;
        }
    }
    return false;
}

// returns false when the window should close
bool HandleEvent(WindowState& w, Apple2& a, const WindowOptions& options, const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return false;
    case SDL_EVENT_KEY_DOWN: {
        SDL_Keycode key = e.key.key;
        if (key == SDLK_F2 && !e.key.repeat) {
            w.look = static_cast<VideoLook>((static_cast<int>(w.look) + 1) % 3);
            UpdateTitle(w, a, options);
            return true;
        }
        if (key == SDLK_F1 && !e.key.repeat) {
            w.turbo = !w.turbo;
            UpdateTitle(w, a, options);
            return true;
        }
        if (key == SDLK_LALT || key == SDLK_RALT) {
            SetButton(a, key == SDLK_LALT ? 0 : 1, true); // open and closed apple
            return true;
        }
        uint8_t code = 0;
        if (SpecialKeyCode(key, code)) {
            PressKey(a, code);
        } else if ((e.key.mod & SDL_KMOD_CTRL) != 0 && key >= SDLK_A && key <= SDLK_Z) {
            PressKey(a, static_cast<uint8_t>((key - SDLK_A + 'A') & 0x1F));
        }
        return true;
    }
    case SDL_EVENT_KEY_UP:
        if (e.key.key == SDLK_LALT || e.key.key == SDLK_RALT) {
            SetButton(a, e.key.key == SDLK_LALT ? 0 : 1, false);
        } else if (!AnyKeyHeld()) {
            ReleaseKey(a);
        }
        return true;
    case SDL_EVENT_TEXT_INPUT: {
        // printable keys arrive as text so the host keyboard layout applies
        for (const char* p = e.text.text; *p != '\0'; p++) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c < 0x20 || c > 0x7E) {
                continue;
            }
            if (c >= 'a' && c <= 'z') {
                c = static_cast<unsigned char>(c - 32);
            }
            PressKey(a, c);
        }
        return true;
    }
    default:
        return true;
    }
}

void RunForHostTime(WindowState& w, Apple2& a, const WindowOptions& options) {
    uint64_t now_ns = SDL_GetTicksNS();
    uint64_t limit = options.max_cycles == 0 ? UINT64_MAX : options.max_cycles;
    if (w.turbo) {
        while (SDL_GetTicksNS() - now_ns < kTurboSliceNs && Cycles(a) < limit && !a.stopped) {
            RunApple2(a, kCyclesPerFrame);
        }
        SkipAudio(w, a);
        w.base_ns = SDL_GetTicksNS();
        w.base_cycle = Cycles(a);
        return;
    }
    double elapsed = static_cast<double>(now_ns - w.base_ns) / kNsPerSecond;
    uint64_t target = w.base_cycle + static_cast<uint64_t>(elapsed * kCpuHz);
    uint64_t now = Cycles(a);
    if (target > now + static_cast<uint64_t>(kCpuHz / 10)) {
        // fell behind (a stall, a dragged window): resync instead of racing to catch up
        w.base_ns = now_ns;
        w.base_cycle = now;
        target = now + kCyclesPerFrame;
    }
    if (target > limit) {
        target = limit;
    }
    if (target > now) {
        RunApple2(a, target - now);
    }
    FeedAudio(w, a);
}

void Present(WindowState& w, const Apple2& a) {
    RenderFrame(a.video.done, w.look, w.fb);
    SDL_UpdateTexture(w.texture, nullptr, w.fb.pixels.data(), kScreenWidth * 4);
    SDL_SetRenderDrawColor(w.renderer, 0, 0, 0, 255);
    SDL_RenderClear(w.renderer);
    SDL_RenderTexture(w.renderer, w.texture, nullptr, nullptr);
    SDL_RenderPresent(w.renderer);
}

void Close(WindowState& w) {
    if (w.audio != nullptr) {
        SDL_DestroyAudioStream(w.audio);
    }
    if (w.texture != nullptr) {
        SDL_DestroyTexture(w.texture);
    }
    if (w.renderer != nullptr) {
        SDL_DestroyRenderer(w.renderer);
    }
    if (w.window != nullptr) {
        SDL_DestroyWindow(w.window);
    }
    SDL_Quit();
}

bool Open(WindowState& w, const WindowOptions& options) {
    // a fixed app id so window managers can match the window (WM_CLASS "tisple")
    SDL_SetAppMetadata("Tisple ][", "0.1", "tisple");
    SDL_SetHint(SDL_HINT_APP_ID, "tisple");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "tisple machine: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    int width = kScreenWidth * options.scale;
    int height = kScreenHeight * 2 * options.scale;
    w.window = SDL_CreateWindow(options.title.c_str(), width, height, SDL_WINDOW_RESIZABLE);
    if (w.window == nullptr) {
        std::fprintf(stderr, "tisple machine: cannot open a window: %s\n", SDL_GetError());
        return false;
    }
    w.renderer = SDL_CreateRenderer(w.window, nullptr);
    if (w.renderer == nullptr) {
        std::fprintf(stderr, "tisple machine: no renderer: %s\n", SDL_GetError());
        return false;
    }
    w.vsync = SDL_SetRenderVSync(w.renderer, 1);
    SDL_SetRenderLogicalPresentation(w.renderer, kScreenWidth, kScreenHeight * 2,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    w.texture = SDL_CreateTexture(w.renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                  kScreenWidth, kScreenHeight);
    if (w.texture == nullptr) {
        std::fprintf(stderr, "tisple machine: no texture: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(w.texture, SDL_SCALEMODE_NEAREST);
    SDL_StartTextInput(w.window);

    SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, kAudioRate};
    w.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (w.audio == nullptr) {
        std::fprintf(stderr, "tisple machine: no audio (%s); running silent\n", SDL_GetError());
    } else {
        PushSilence(w, kLatencyTargetMs);
        SDL_ResumeAudioStreamDevice(w.audio);
    }
    return true;
}

} // namespace

int RunMachineWindow(Apple2& a, const WindowOptions& options) {
    WindowState w;
    w.look = options.look;
    if (!Open(w, options)) {
        Close(w);
        return 1;
    }
    SkipAudio(w, a);
    w.base_ns = SDL_GetTicksNS();
    w.base_cycle = Cycles(a);
    for (;;) {
        uint64_t frame_start = SDL_GetTicksNS();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (!HandleEvent(w, a, options, e)) {
                Close(w);
                return 0;
            }
        }
        RunForHostTime(w, a, options);
        if (a.stopped && !w.reported_stop) {
            w.reported_stop = true;
            std::printf("stopped: %s\n", DescribeStop(a).c_str());
            std::fputs(DescribeStubNotes(a.stub_notes).c_str(), stdout);
            std::fflush(stdout);
            UpdateTitle(w, a, options);
        }
        Present(w, a);
        if (!w.vsync) {
            uint64_t spent = SDL_GetTicksNS() - frame_start;
            uint64_t frame_ns = static_cast<uint64_t>(kNsPerSecond * kCyclesPerFrame / kCpuHz);
            if (spent < frame_ns) {
                SDL_DelayNS(frame_ns - spent);
            }
        }
    }
}

} // namespace tisple
