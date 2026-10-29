#include "machine/machine_command.hpp"

#include "disk/machine_disks.hpp"
#include "machine/disk2.hpp"
#include "machine/mouse.hpp"
#include "machine/png.hpp"
#include "machine/scripted_input.hpp"
#include "machine/speaker_audio.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

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
    const char* mouse = nullptr;     // scripted mouse input
    const char* mouse_rom = nullptr; // the owner's MouseInterface.rom; null for ours
    uint32_t origin = kDefaultOrigin;
    uint64_t cycles = kDefaultCycles;
    bool cycles_given = false;
    VideoLook look = VideoLook::kColor;
    bool window = false;
    int scale = 2;
    std::vector<ScreenshotArg> screenshots;
    std::vector<std::pair<uint32_t, uint32_t>> peeks; // address, byte count
    MachineDisks disks;
};

void PrintMachineUsage() {
    std::fprintf(
        stderr,
        "usage: tisple machine <file.bin> [options]\n"
        "       tisple machine --disk a.dsk [--disk2 b.dsk] [options]   boot from a disk\n"
        "  --origin A          load address and entry (default $6000)\n"
        "  --cycles N | --frames N   how long to run (default 100000000 cycles)\n"
        "  --keys \"C:K,...\"    press key K at cycle C; K is a character, $hex, or one of\n"
        "                      RETURN ESC LEFT RIGHT UP DOWN SPACE TAB DELETE COMMA\n"
        "  --screenshot F.png[@C]  save the screen at cycle C (default: at the end)\n"
        "  --wav F.wav         save the speaker\n"
        "  --ntsc | --mono     composite ntsc look, or a monochrome monitor (default: flat color)\n"
        "  --rom F             a 12 or 16 KB rom image; with no program, reset through it\n"
        "  --disk F | --disk2 F  a disk ii in slot 6 with F (.dsk .do .po .nib) in drive 1 / 2;\n"
        "                      with no program and no --rom, boot it with our boot rom\n"
        "  --disk-rom F        the card's boot rom (Apple's DISK2.rom) instead of ours\n"
        "  --mouse \"C:X,Y,B;...\"  the mouse card (slot 4) is at X,Y with the button B (0 or\n"
        "                      1) from cycle C\n"
        "  --mouse-rom F       the card's real firmware (MouseInterface.rom) instead of ours;\n"
        "                      it needs an Apple rom too (--rom)\n"
        "  --write-back        save written disks back to their files at the end\n"
        "  --load-snapshot F | --save-snapshot F   machine state in and out\n"
        "  --peek A[:N]        print N bytes (default 2, as a u16 too) at A after the run\n"
        "  --window [--scale N]  run in a window at real speed (F1: unthrottled)\n"
        "numbers take $hex, 0xhex or decimal; a cycle may be given in frames as 30f\n"
        "exit code 3 when the program hits an illegal opcode or jumps into empty rom space\n");
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
            args.look = VideoLook::kMono;
        } else if (std::strcmp(arg, "--ntsc") == 0) {
            args.look = VideoLook::kNtsc;
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
        } else if (std::strcmp(arg, "--mouse") == 0 && has_value) {
            args.mouse = argv[++i];
        } else if (std::strcmp(arg, "--mouse-rom") == 0 && has_value) {
            args.mouse_rom = argv[++i];
        } else if (std::strcmp(arg, "--screenshot") == 0 && has_value) {
            ScreenshotArg shot;
            if (!ParseScreenshot(argv[++i], shot)) {
                return false;
            }
            args.screenshots.push_back(shot);
        } else if (std::strcmp(arg, "--peek") == 0 && has_value) {
            std::string text = argv[++i];
            size_t colon = text.find(':');
            uint64_t count = 2;
            if (!ParseNumber(text.substr(0, colon).c_str(), number) || number > 0xFFFF ||
                (colon != std::string::npos &&
                 (!ParseNumber(text.substr(colon + 1).c_str(), count) || count == 0))) {
                return false;
            }
            args.peeks.push_back({static_cast<uint32_t>(number), static_cast<uint32_t>(count)});
        } else if (std::strcmp(arg, "--wav") == 0 && has_value) {
            args.wav = argv[++i];
        } else if (std::strcmp(arg, "--rom") == 0 && has_value) {
            args.rom = argv[++i];
        } else if (std::strcmp(arg, "--disk") == 0 && has_value) {
            args.disks.paths[0] = argv[++i];
        } else if (std::strcmp(arg, "--disk2") == 0 && has_value) {
            args.disks.paths[1] = argv[++i];
        } else if (std::strcmp(arg, "--disk-rom") == 0 && has_value) {
            args.disks.rom_path = argv[++i];
        } else if (std::strcmp(arg, "--write-back") == 0) {
            args.disks.write_back = true;
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

// the mouse card in slot 4, with our firmware or the owner's
bool SetUpMouse(Apple2& a, const MachineArgs& args, std::string& error) {
    if (args.mouse_rom == nullptr) {
        InsertMouseCard(a, kMouseSlot, nullptr);
        return true;
    }
    if (args.rom == nullptr) {
        error = "the real mouse firmware calls the monitor rom (IORTS at $FF58); add "
                "--rom Apple2e.rom";
        return false;
    }
    std::vector<uint8_t> rom;
    if (!ReadFile(args.mouse_rom, rom)) {
        error = std::string("cannot read '") + args.mouse_rom + "'";
        return false;
    }
    if (rom.size() != kMouseRomBytes) {
        error = "MouseInterface.rom is 2048 bytes, not " + std::to_string(rom.size());
        return false;
    }
    InsertMouseCard(a, kMouseSlot, rom.data());
    return true;
}

bool SaveShot(const Apple2& a, const ScreenshotArg& shot, VideoLook look, bool live) {
    Framebuffer fb;
    if (live) {
        RenderFrame(a.video.live, look, fb);
    } else {
        RenderScreen(a, look, fb);
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
                ok = SaveShot(a, shot, args.look, true) && ok;
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
            ok = SaveShot(a, shot, args.look, false) && ok;
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
    for (const auto& peek : args.peeks) {
        std::printf("peek $%04X:", peek.first);
        for (uint32_t k = 0; k < peek.second; k++) {
            std::printf(" %02X", PeekMemory(a, static_cast<uint16_t>(peek.first + k)));
        }
        if (peek.second == 2) {
            std::printf("  (u16 %u)",
                        PeekMemory(a, static_cast<uint16_t>(peek.first)) |
                            (PeekMemory(a, static_cast<uint16_t>(peek.first + 1)) << 8));
        }
        std::printf("\n");
    }
    std::fputs(DescribeDisks(a).c_str(), stdout);
    std::fputs(DescribeMouse(a).c_str(), stdout);
    if (!ok) {
        return 1;
    }
    bool crashed = a.stopped && (a.stop_reason == MachineStop::kIllegalOpcode ||
                                 a.stop_reason == MachineStop::kNoRom);
    return crashed ? 3 : 0;
}

// rom, snapshot or program into a fresh machine; then keys, then run
int RunWithArgs(const std::vector<uint8_t>* program, const std::vector<MemorySegment>& segments,
                const MachineArgs& args, MachineWindowFn window) {
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
    // a snapshot carries its cards and disks; --disk then only names the write-back files
    if (args.disks.any() && args.load_snapshot == nullptr && !SetUpDisks(*a, args.disks, error)) {
        std::fprintf(stderr, "tisple machine: %s\n", error.c_str());
        return 1;
    }
    if (args.load_snapshot == nullptr && !SetUpMouse(*a, args, error)) {
        std::fprintf(stderr, "tisple machine: %s\n", error.c_str());
        return 1;
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
        for (const MemorySegment& segment : segments) {
            if (!LoadSegment(*a, segment, error)) {
                std::fprintf(stderr, "tisple machine: %s\n", error.c_str());
                return 1;
            }
        }
        if (!LoadProgram(*a, program->data(), program->size(), static_cast<uint16_t>(args.origin),
                         error)) {
            std::fprintf(stderr, "tisple machine: %s\n", error.c_str());
            return 1;
        }
    } else if (args.rom != nullptr) {
        ResetApple2(*a); // the rom's autostart boots slot 6 when a disk is in
    } else if (args.disks.any()) {
        StartDiskBoot(*a);
    } else {
        PrintMachineUsage();
        return 1;
    }
    if (args.keys != nullptr && !ScheduleKeys(*a, args.keys, error)) {
        std::fprintf(stderr, "tisple machine: %s\n", error.c_str());
        return 1;
    }
    if (args.mouse != nullptr && !ScheduleMouseInput(*a, args.mouse, error)) {
        std::fprintf(stderr, "tisple machine: %s\n", error.c_str());
        return 1;
    }
    int code = 0;
    if (!args.window) {
        code = RunHeadless(*a, args);
    } else if (window == nullptr) {
        std::fprintf(
            stderr,
            "tisple machine: this build has no window (built without SDL3, TISPLE_SDL off)\n");
        return 1;
    } else {
        WindowOptions options;
        options.scale = args.scale;
        options.look = args.look;
        options.max_cycles = args.cycles_given ? args.cycles : 0;
        if (args.program != nullptr) {
            options.title = std::string("Tisple ][ - ") + args.program;
        } else if (!args.disks.paths[0].empty()) {
            options.title = "Tisple ][ - " + args.disks.paths[0];
        }
        code = window(*a, options);
    }
    if (args.disks.write_back) {
        std::string report;
        bool saved = WriteBackDisks(*a, args.disks, report);
        std::fputs(report.c_str(), stdout);
        code = saved ? code : 1;
    }
    return code;
}

} // namespace

int RunMachineCommand(int argc, char** argv, MachineWindowFn window) {
    MachineArgs args;
    if (!ParseArgs(argc, argv, true, args)) {
        PrintMachineUsage();
        return 1;
    }
    if (args.program == nullptr) {
        return RunWithArgs(nullptr, {}, args, window);
    }
    std::vector<uint8_t> program;
    if (!ReadFile(args.program, program)) {
        std::fprintf(stderr, "tisple machine: cannot read '%s'\n", args.program);
        return 1;
    }
    return RunWithArgs(&program, {}, args, window);
}

int RunMachineOnDisk(const std::vector<uint8_t>& image, DiskFormat format, int argc, char** argv,
                     MachineWindowFn window) {
    MachineArgs args;
    if (!ParseArgs(argc, argv, false, args)) {
        PrintMachineUsage();
        return 1;
    }
    args.disks.images[0] = image;
    args.disks.formats[0] = format;
    return RunWithArgs(nullptr, {}, args, window);
}

int RunMachineOnBinary(const std::vector<uint8_t>& binary, uint16_t origin,
                       const std::vector<MemorySegment>& segments, int argc, char** argv,
                       MachineWindowFn window) {
    MachineArgs args;
    if (!ParseArgs(argc, argv, false, args)) {
        PrintMachineUsage();
        return 1;
    }
    args.origin = origin;
    return RunWithArgs(&binary, segments, args, window);
}

} // namespace tisple
