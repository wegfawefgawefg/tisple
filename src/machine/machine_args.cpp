#include "machine/machine_args.hpp"

#include "machine/scripted_input.hpp"

#include <cstdio>
#include <cstring>

namespace tisple {

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
        "  --wav F.wav         save the speaker, and the Mockingboard's sound chips when it is in\n"
        "  --ntsc | --mono     composite ntsc look, or a monochrome monitor (default: flat color)\n"
        "  --rom F             a 12 or 16 KB rom image; with no program, reset through it\n"
        "  --disk F | --disk2 F  a disk ii in slot 6 with F (.dsk .do .po .nib) in drive 1 / 2;\n"
        "                      with no program and no --rom, boot it with our boot rom\n"
        "  --disk-rom F        the card's boot rom (Apple's DISK2.rom) instead of ours\n"
        "  --mouse \"C:X,Y,B;...\"  the mouse card (slot 4) is at X,Y with the button B (0 or\n"
        "                      1) from cycle C\n"
        "  --mouse-rom F       the card's real firmware (MouseInterface.rom) instead of ours;\n"
        "                      it needs an Apple rom too (--rom)\n"
        "  --mockingboard N    a Mockingboard in slot N (1-7; 4 is usual: then no mouse card);\n"
        "                      its interrupts need an Apple rom (--rom) unless the program\n"
        "                      runs in the language card\n"
        "  --write-back        save written disks back to their files at the end\n"
        "  --load-snapshot F | --save-snapshot F   machine state in and out\n"
        "  --peek A[:N]        print N bytes (default 2, as a u16 too) at A after the run\n"
        "  --window [--scale N]  run in a window at real speed (F1: unthrottled)\n"
        "  --taps F              in the window, clicks press keys from F's \"taps\" boxes\n"
        "                      (a game's phone player config, docs/phone.md)\n"
        "numbers take $hex, 0xhex or decimal; a cycle may be given in frames as 30f\n"
        "exit code 3 when the program hits an illegal opcode or jumps into empty rom space\n");
}

namespace {

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

} // namespace

bool ParseMachineArgs(int argc, char** argv, bool want_program, MachineArgs& args) {
    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];
        bool has_value = i + 1 < argc;
        uint64_t number = 0;
        if (std::strcmp(arg, "--mono") == 0) {
            args.look = VideoLook::kMono;
        } else if (std::strcmp(arg, "--ntsc") == 0) {
            args.look = VideoLook::kNtsc;
        } else if (std::strcmp(arg, "--taps") == 0 && has_value) {
            args.taps = argv[++i];
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
        } else if (std::strcmp(arg, "--mockingboard") == 0 && has_value) {
            if (!ParseNumber(argv[++i], number) || number < 1 || number > 7) {
                return false;
            }
            args.mockingboard = static_cast<int>(number);
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

} // namespace tisple
