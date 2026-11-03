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
        "  --wav F.wav         save the speaker, the Mockingboard's sound chips when it is in,\n"
        "                      and the disk drives' sound (steps, the motor), in stereo: the\n"
        "                      card's first chip left, its second right, the rest in both\n"
        "  --mono-sound        --wav and the window in one channel, everything mixed in it\n"
        "  --no-drive-sound    leave the drives' sound out of --wav and the window\n"
        "  --ntsc | --mono     composite ntsc look, or a monochrome monitor (default: flat color;\n"
        "                      --mono is the picture: the sound is --mono-sound)\n"
        "  --rom F             a 12 or 16 KB rom image; with no program, reset through it\n"
        "  --disk F | --disk2 F  a disk ii in slot 6 with F (.dsk .do .po .nib) in drive 1 / 2;\n"
        "                      with no program and no --rom, boot it with our boot rom\n"
        "  --drives 1          one drive: nothing answers on drive 2 (no disk, no noise)\n"
        "  --swap C:F          at cycle C put F in drive 1, as a person swapping disks\n"
        "                      would (headless; any number of them, in order)\n"
        "  --disk-rom F        the card's boot rom (Apple's DISK2.rom) instead of ours\n"
        "  --mouse \"C:X,Y,B;...\"  the mouse card (slot 4) is at X,Y with the button B (0 or\n"
        "                      1) from cycle C\n"
        "  --taps-at \"C:X,Y,B;...\"  a tap (B 1) or lift (B 0) at X,Y at cycle C, as the\n"
        "                      phone player makes them: replays a playtest log's taps\n"
        "  --joystick \"C:X,Y,B;...\"  from cycle C paddles 0 and 1 read X and Y (0-255) and\n"
        "                      the buttons are down by B's bits (1 open-apple, 2 closed-apple,\n"
        "                      4 button 2)\n"
        "  --model M           with no rom, the Apple II it says it is to a program that asks:\n"
        "                      iie-enhanced (default), iie, iic, iiplus, iigs\n"
        "  --cpu C             6502 or 65c02 (default: the model's, or the rom's: the 65C02 on\n"
        "                      the enhanced iie, iic and iigs, the 6502 on the iie and ii+)\n"
        "  --64k               no extended 80-column card: a 64 KB iie\n"
        "  --mouse-rom F       the card's real firmware (MouseInterface.rom) instead of ours;\n"
        "                      it needs an Apple rom too (--rom)\n"
        "  --mockingboard N    a Mockingboard in slot N (1-7; 4 is usual: then no mouse card);\n"
        "                      its interrupts need an Apple rom (--rom) unless the program\n"
        "                      runs in the language card\n"
        "  --write-back        save written disks back to their files at the end\n"
        "  --load-snapshot F | --save-snapshot F   machine state in and out\n"
        "  --peek A[:N]        print N bytes (default 2, as a u16 too) at A after the run\n"
        "  --profile F.txt[@C] cycles by instruction address and memory, from cycle C to\n"
        "                      the end (tools/profile.py adds them up by function)\n"
        "  --window [--scale N]  run in a window at real speed (F1: unthrottled, F2: the\n"
        "                      look, F3: the drives' sound on and off, F4: stereo or mono)\n"
        "  --taps F              in the window, clicks press keys from F's \"taps\" boxes\n"
        "                      (with \"mouse\": true in F, a click is the mouse card's mouse\n"
        "                      there, pressed)\n"
        "                      (a game's phone player config, docs/phone.md)\n"
        "numbers take $hex, 0xhex or decimal; a cycle may be given in frames as 30f\n"
        "exit code 3 when the program hits an illegal opcode, jams the cpu or jumps into\n"
        "empty rom space\n");
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

Cpu MachineCpu(const Apple2& a, const MachineArgs& args) {
    if (args.cpu_given) {
        return args.cpu;
    }
    MachineModel model = args.model;
    RomModel(a, model);
    return ModelCpu(model);
}

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
        } else if (std::strcmp(arg, "--no-drive-sound") == 0) {
            args.drive_sound = false;
        } else if (std::strcmp(arg, "--mono-sound") == 0) {
            args.mono_sound = true;
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
        } else if (std::strcmp(arg, "--taps-at") == 0 && has_value) {
            args.taps_at = argv[++i];
        } else if (std::strcmp(arg, "--joystick") == 0 && has_value) {
            args.joystick = argv[++i];
        } else if (std::strcmp(arg, "--model") == 0 && has_value) {
            if (!ParseModel(argv[++i], args.model)) {
                return false;
            }
        } else if (std::strcmp(arg, "--cpu") == 0 && has_value) {
            if (!ParseCpu(argv[++i], args.cpu)) {
                return false;
            }
            args.cpu_given = true;
        } else if (std::strcmp(arg, "--64k") == 0) {
            args.aux_ram = false;
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
        } else if (std::strcmp(arg, "--profile") == 0 && has_value) {
            if (!ParseScreenshot(argv[++i], args.profile)) {
                return false;
            }
        } else if (std::strcmp(arg, "--wav") == 0 && has_value) {
            args.wav = argv[++i];
        } else if (std::strcmp(arg, "--rom") == 0 && has_value) {
            args.rom = argv[++i];
        } else if (std::strcmp(arg, "--disk") == 0 && has_value) {
            args.disks.paths[0] = argv[++i];
        } else if (std::strcmp(arg, "--disk2") == 0 && has_value) {
            args.disks.paths[1] = argv[++i];
        } else if (std::strcmp(arg, "--drives") == 0 && has_value) {
            std::string n = argv[++i];
            if (n != "1" && n != "2") {
                return false;
            }
            args.disks.drives = n == "1" ? 1 : 2;
        } else if (std::strcmp(arg, "--swap") == 0 && has_value) {
            std::string text = argv[++i];
            size_t colon = text.find(':');
            MachineDisks::Swap swap;
            if (colon == std::string::npos || !ParseCycle(text.substr(0, colon), swap.cycle)) {
                return false;
            }
            swap.path = text.substr(colon + 1);
            args.disks.swaps.push_back(swap);
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
