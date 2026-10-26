#include "machine/machine_command.hpp"

#include "machine/png.hpp"
#include "machine/speaker_audio.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace tisple {

namespace {

constexpr uint64_t kDefaultCycles = 100000000; // about 98 seconds of apple time

struct ScreenshotArg {
    std::string path;
    uint64_t cycle = 0;
    bool at_end = true;
};

struct MachineArgs {
    const char* program = nullptr;
    const char* rom = nullptr;
    const char* load_snapshot = nullptr;
    const char* save_snapshot = nullptr;
    const char* wav = nullptr;
    const char* keys = nullptr;
    uint32_t origin = kDefaultOrigin;
    uint64_t cycles = kDefaultCycles;
    bool cycles_given = false;
    bool mono = false;
    bool window = false;
    int scale = 2;
    std::vector<ScreenshotArg> screenshots;
};

void PrintMachineUsage() {
    std::fprintf(
        stderr,
        "usage: tisple machine <file.bin> [options]\n"
        "  --origin A          load address and entry (default $6000)\n"
        "  --cycles N | --frames N   how long to run (default 100000000 cycles)\n"
        "  --keys \"C:K,...\"    press key K at cycle C; K is a character, $hex, or one of\n"
        "                      RETURN ESC LEFT RIGHT UP DOWN SPACE TAB DELETE COMMA\n"
        "  --screenshot F.png[@C]  save the screen at cycle C (default: at the end)\n"
        "  --wav F.wav         save the speaker\n"
        "  --mono              monochrome monitor\n"
        "  --rom F             a 12 or 16 KB rom image; with no program, reset through it\n"
        "  --load-snapshot F | --save-snapshot F   machine state in and out\n"
        "  --window [--scale N]  run in a window at real speed (F1: unthrottled)\n"
        "numbers take $hex, 0xhex or decimal; a cycle may be given in frames as 30f\n"
        "exit code 3 when the program hits an illegal opcode or jumps into empty rom space\n");
}

bool ParseNumber(const char* text, uint64_t& out) {
    char* end = nullptr;
    if (text[0] == '$') {
        out = std::strtoull(text + 1, &end, 16);
        return text[1] != '\0' && *end == '\0';
    }
    out = std::strtoull(text, &end, 0);
    return text[0] != '\0' && *end == '\0';
}

// a number of cycles, or of frames with an f suffix
bool ParseCycle(const std::string& text, uint64_t& out) {
    if (!text.empty() && text.back() == 'f') {
        uint64_t frames = 0;
        if (!ParseNumber(text.substr(0, text.size() - 1).c_str(), frames)) {
            return false;
        }
        out = frames * kCyclesPerFrame;
        return true;
    }
    return ParseNumber(text.c_str(), out);
}

bool ParseKeyName(const std::string& name, uint8_t& code) {
    struct Named {
        const char* name;
        uint8_t code;
    };
    static const Named kNames[] = {
        {"RETURN", 0x0D}, {"RET", 0x0D},    {"ESC", 0x1B},  {"LEFT", 0x08},
        {"RIGHT", 0x15},  {"UP", 0x0B},     {"DOWN", 0x0A}, {"SPACE", 0x20},
        {"TAB", 0x09},    {"DELETE", 0x7F}, {"DEL", 0x7F},  {"COMMA", 0x2C},
    };
    for (const Named& n : kNames) {
        if (name == n.name) {
            code = n.code;
            return true;
        }
    }
    if (name.size() == 1) {
        char c = name[0];
        code = static_cast<uint8_t>(c >= 'a' && c <= 'z' ? c - 32 : c);
        return true;
    }
    uint64_t value = 0;
    if ((name[0] == '$' || name.rfind("0x", 0) == 0) && ParseNumber(name.c_str(), value) &&
        value < 0x80) {
        code = static_cast<uint8_t>(value);
        return true;
    }
    return false;
}

bool ScheduleKeys(Apple2& a, const char* spec, std::string& error) {
    std::string text = spec;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t comma = text.find(',', pos);
        std::string entry =
            text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? text.size() : comma + 1;
        size_t colon = entry.find(':');
        uint64_t cycle = 0;
        uint8_t code = 0;
        if (colon == std::string::npos || !ParseCycle(entry.substr(0, colon), cycle) ||
            !ParseKeyName(entry.substr(colon + 1), code)) {
            error = "bad key '" + entry + "', want cycle:key";
            return false;
        }
        if (!ScheduleKey(a, cycle, code)) {
            error = "too many scripted keys";
            return false;
        }
    }
    return true;
}

bool ParseScreenshot(const char* text, ScreenshotArg& shot) {
    std::string arg = text;
    size_t at = arg.rfind('@');
    if (at == std::string::npos) {
        shot.path = arg;
        return !arg.empty();
    }
    shot.path = arg.substr(0, at);
    shot.at_end = false;
    return !shot.path.empty() && ParseCycle(arg.substr(at + 1), shot.cycle);
}

bool ParseArgs(int argc, char** argv, bool want_program, MachineArgs& args) {
    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];
        bool has_value = i + 1 < argc;
        uint64_t number = 0;
        if (std::strcmp(arg, "--mono") == 0) {
            args.mono = true;
        } else if (std::strcmp(arg, "--window") == 0) {
            args.window = true;
        } else if (std::strcmp(arg, "--origin") == 0 && has_value) {
            if (!ParseNumber(argv[++i], number) || number > 0xBFFF) {
                return false;
            }
            args.origin = static_cast<uint32_t>(number);
        } else if (std::strcmp(arg, "--cycles") == 0 && has_value) {
            if (!ParseNumber(argv[++i], args.cycles)) {
                return false;
            }
            args.cycles_given = true;
        } else if (std::strcmp(arg, "--frames") == 0 && has_value) {
            if (!ParseNumber(argv[++i], number)) {
                return false;
            }
            args.cycles = number * kCyclesPerFrame;
            args.cycles_given = true;
        } else if (std::strcmp(arg, "--scale") == 0 && has_value) {
            if (!ParseNumber(argv[++i], number) || number < 1 || number > 8) {
                return false;
            }
            args.scale = static_cast<int>(number);
        } else if (std::strcmp(arg, "--keys") == 0 && has_value) {
            args.keys = argv[++i];
        } else if (std::strcmp(arg, "--screenshot") == 0 && has_value) {
            ScreenshotArg shot;
            if (!ParseScreenshot(argv[++i], shot)) {
                return false;
            }
            args.screenshots.push_back(shot);
        } else if (std::strcmp(arg, "--wav") == 0 && has_value) {
            args.wav = argv[++i];
        } else if (std::strcmp(arg, "--rom") == 0 && has_value) {
            args.rom = argv[++i];
        } else if (std::strcmp(arg, "--load-snapshot") == 0 && has_value) {
            args.load_snapshot = argv[++i];
        } else if (std::strcmp(arg, "--save-snapshot") == 0 && has_value) {
            args.save_snapshot = argv[++i];
        } else if (want_program && arg[0] != '-' && args.program == nullptr) {
            args.program = arg;
        } else {
            return false;
        }
    }
    return true;
}

bool ReadFile(const char* path, std::vector<uint8_t>& bytes) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    uint8_t chunk[4096];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + got);
    }
    std::fclose(file);
    return true;
}

bool WriteFile(const char* path, const std::vector<uint8_t>& bytes) {
    FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        return false;
    }
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && ok;
}

bool SaveShot(const Apple2& a, const ScreenshotArg& shot, bool mono, bool live) {
    Framebuffer fb;
    if (live) {
        RenderFrame(a.video.live, mono, fb);
    } else {
        RenderScreen(a, mono, fb);
    }
    std::string error;
    if (!WriteScreenPng(shot.path, fb, error)) {
        std::fprintf(stderr, "tisple machine: %s\n", error.c_str());
        return false;
    }
    return true;
}

// the next cycle something must happen at: a screenshot or the end
uint64_t NextStop(const MachineArgs& args, const std::vector<bool>& taken, uint64_t end) {
    uint64_t next = end;
    for (size_t i = 0; i < args.screenshots.size(); i++) {
        const ScreenshotArg& shot = args.screenshots[i];
        if (!shot.at_end && !taken[i] && shot.cycle < next) {
            next = shot.cycle;
        }
    }
    return next;
}

int RunHeadless(Apple2& a, const MachineArgs& args) {
    uint64_t start = Cycles(a);
    uint64_t end = start + args.cycles;
    std::vector<bool> taken(args.screenshots.size(), false);
    SpeakerRenderer speaker;
    speaker.next_toggle = a.speaker.toggle_count;
    speaker.samples_done = SamplesForCycles(start);
    std::vector<int16_t> samples;
    bool ok = true;
    uint64_t run_cycles = 0;

    auto began = std::chrono::steady_clock::now();
    for (;;) {
        uint64_t now = Cycles(a);
        // take every screenshot due now
        for (size_t i = 0; i < args.screenshots.size(); i++) {
            const ScreenshotArg& shot = args.screenshots[i];
            if (!shot.at_end && !taken[i] && shot.cycle <= now) {
                CatchUpVideo(a, now);
                ok = SaveShot(a, shot, args.mono, true) && ok;
                taken[i] = true;
            }
        }
        uint64_t next = NextStop(args, taken, end);
        if (now >= next) {
            break;
        }
        // once the program stops, only pending screenshots keep the clock going
        if (a.stopped && next == end) {
            break;
        }
        uint64_t chunk = next - now;
        chunk = chunk > kCyclesPerFrame ? kCyclesPerFrame : chunk;
        bool was_stopped = a.stopped;
        RunOutcome outcome = RunApple2(a, chunk);
        if (!was_stopped) {
            run_cycles += outcome.cycles;
        }
        if (args.wav != nullptr && !was_stopped) {
            RenderSpeaker(speaker, a, Cycles(a), samples);
        }
    }
    double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

    for (const ScreenshotArg& shot : args.screenshots) {
        if (shot.at_end) {
            ok = SaveShot(a, shot, args.mono, false) && ok;
        }
    }
    std::string error;
    if (args.wav != nullptr && !WriteWav(args.wav, samples, error)) {
        std::fprintf(stderr, "tisple machine: %s\n", error.c_str());
        ok = false;
    }
    if (args.save_snapshot != nullptr && !WriteFile(args.save_snapshot, SaveSnapshot(a))) {
        std::fprintf(stderr, "tisple machine: cannot write '%s'\n", args.save_snapshot);
        ok = false;
    }

    if (a.stopped) {
        std::printf("stopped: %s\n", DescribeStop(a).c_str());
    } else {
        std::printf("stopped: cycle limit at $%04X\n", GetRegs(a.cpu).pc);
    }
    std::printf("cycles: %llu (%.2f frames)\n", static_cast<unsigned long long>(run_cycles),
                static_cast<double>(run_cycles) / kCyclesPerFrame);
    if (args.wav != nullptr) {
        std::printf("speaker: %llu toggles, %zu samples\n",
                    static_cast<unsigned long long>(a.speaker.toggle_count), samples.size());
    }
    if (seconds > 0 && run_cycles > 0) {
        double rate = static_cast<double>(run_cycles) / seconds;
        std::printf("speed: %.0fx real time (%.1f M cycles/s)\n", rate / kCpuHz, rate / 1e6);
    }
    std::fputs(DescribeStubNotes(a.stub_notes).c_str(), stdout);
    if (!ok) {
        return 1;
    }
    bool crashed = a.stopped && (a.stop_reason == MachineStop::kIllegalOpcode ||
                                 a.stop_reason == MachineStop::kNoRom);
    return crashed ? 3 : 0;
}

// rom, snapshot or program into a fresh machine; then keys, then run
int RunWithArgs(const std::vector<uint8_t>* program, const MachineArgs& args,
                MachineWindowFn window) {
    std::unique_ptr<Apple2> a = NewApple2();
    std::string error;
    if (args.rom != nullptr) {
        std::vector<uint8_t> rom;
        if (!ReadFile(args.rom, rom)) {
            std::fprintf(stderr, "tisple machine: cannot read '%s'\n", args.rom);
            return 1;
        }
        if (!LoadRom(*a, rom.data(), rom.size(), error)) {
            std::fprintf(stderr, "tisple machine: %s\n", error.c_str());
            return 1;
        }
    }
    if (args.load_snapshot != nullptr) {
        std::vector<uint8_t> bytes;
        if (!ReadFile(args.load_snapshot, bytes) ||
            !LoadSnapshot(*a, bytes.data(), bytes.size(), error)) {
            std::fprintf(stderr, "tisple machine: cannot load snapshot '%s' %s\n",
                         args.load_snapshot, error.c_str());
            return 1;
        }
    } else if (program != nullptr) {
        if (!LoadProgram(*a, program->data(), program->size(), static_cast<uint16_t>(args.origin),
                         error)) {
            std::fprintf(stderr, "tisple machine: %s\n", error.c_str());
            return 1;
        }
    } else if (args.rom != nullptr) {
        ResetApple2(*a);
    } else {
        PrintMachineUsage();
        return 1;
    }
    if (args.keys != nullptr && !ScheduleKeys(*a, args.keys, error)) {
        std::fprintf(stderr, "tisple machine: %s\n", error.c_str());
        return 1;
    }
    if (!args.window) {
        return RunHeadless(*a, args);
    }
    if (window == nullptr) {
        std::fprintf(
            stderr,
            "tisple machine: this build has no window (built without SDL3, TISPLE_SDL off)\n");
        return 1;
    }
    WindowOptions options;
    options.scale = args.scale;
    options.mono = args.mono;
    options.max_cycles = args.cycles_given ? args.cycles : 0;
    if (args.program != nullptr) {
        options.title = std::string("Tisple ][ - ") + args.program;
    }
    return window(*a, options);
}

} // namespace

int RunMachineCommand(int argc, char** argv, MachineWindowFn window) {
    MachineArgs args;
    if (!ParseArgs(argc, argv, true, args)) {
        PrintMachineUsage();
        return 1;
    }
    if (args.program == nullptr) {
        return RunWithArgs(nullptr, args, window);
    }
    std::vector<uint8_t> program;
    if (!ReadFile(args.program, program)) {
        std::fprintf(stderr, "tisple machine: cannot read '%s'\n", args.program);
        return 1;
    }
    return RunWithArgs(&program, args, window);
}

int RunMachineOnBinary(const std::vector<uint8_t>& binary, uint16_t origin, int argc, char** argv,
                       MachineWindowFn window) {
    MachineArgs args;
    if (!ParseArgs(argc, argv, false, args)) {
        PrintMachineUsage();
        return 1;
    }
    args.origin = origin;
    return RunWithArgs(&binary, args, window);
}

} // namespace tisple
