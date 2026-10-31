#include "machine/machine_command.hpp"

#include "disk/machine_disks.hpp"
#include "machine/disk2.hpp"
#include "machine/machine_args.hpp"
#include "machine/mockingboard.hpp"
#include "machine/mockingboard_audio.hpp"
#include "machine/mouse.hpp"
#include "machine/png.hpp"
#include "machine/profile.hpp"
#include "machine/scripted_input.hpp"
#include "machine/speaker_audio.hpp"
#include "machine/taps.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace tisple {

namespace {

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

// the mouse card in slot 4, with our firmware or the owner's, unless the Mockingboard is there
bool SetUpMouse(Apple2& a, const MachineArgs& args, std::string& error) {
    if (args.mockingboard == kMouseSlot) {
        if (args.mouse != nullptr || args.mouse_rom != nullptr) {
            error = "the Mockingboard is in the mouse card's slot 4; put it in another slot "
                    "(--mockingboard 5)";
            return false;
        }
        return true;
    }
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
    MockingboardRenderer chips;
    StartMockingboardAudio(chips, a);
    std::vector<int16_t> samples;
    bool ok = true;
    uint64_t run_cycles = 0;
    bool profiling = !args.profile.path.empty();
    Profile profile;
    if (profiling) {
        StartProfile(profile, a, args.profile.at_end ? start : args.profile.cycle);
    }

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
        RunOutcome outcome = profiling ? RunApple2Traced(a, chunk, ProfileStep, &profile)
                                       : RunApple2(a, chunk);
        if (!was_stopped) {
            run_cycles += outcome.cycles;
        }
        if (args.wav != nullptr && !was_stopped) {
            size_t first = samples.size();
            RenderSpeaker(speaker, a, Cycles(a), samples);
            if (HasMockingboard(a)) {
                MixMockingboard(chips, a, Cycles(a), samples, first);
            }
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
    if (profiling && !WriteProfile(profile, args.profile.path.c_str(), error)) {
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
    std::fputs(DescribeMockingboard(a).c_str(), stdout);
    if (!ok) {
        return 1;
    }
    bool crashed = a.stopped &&
                   (a.stop_reason == MachineStop::kIllegalOpcode ||
                    a.stop_reason == MachineStop::kNoRom || a.stop_reason == MachineStop::kJammed);
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
    if (args.mockingboard == 6 && args.disks.any()) {
        std::fprintf(stderr, "tisple machine: the disk ii card is in slot 6; put the "
                             "Mockingboard in another slot (--mockingboard 4)\n");
        return 1;
    }
    if (args.load_snapshot == nullptr && args.mockingboard != 0) {
        InsertMockingboard(*a, args.mockingboard);
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
        if (args.taps != nullptr && !LoadTaps(args.taps, options.taps, error)) {
            std::fprintf(stderr, "tisple machine: %s\n", error.c_str());
            return 1;
        }
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
    if (!ParseMachineArgs(argc, argv, true, args)) {
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
    if (!ParseMachineArgs(argc, argv, false, args)) {
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
    if (!ParseMachineArgs(argc, argv, false, args)) {
        PrintMachineUsage();
        return 1;
    }
    args.origin = origin;
    return RunWithArgs(&binary, segments, args, window);
}

} // namespace tisple
