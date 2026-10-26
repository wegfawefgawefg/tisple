// tisple build main.tsp --disk out.dsk / tisple run main.tsp --disk: compile, then a
// self-booting disk; run boots it on the built-in machine as a rehearsal of real hardware

#include "backend/backend.hpp"
#include "check/check.hpp"
#include "check/diag.hpp"
#include "disk/disk_command.hpp"
#include "disk/image.hpp"
#include "machine/machine_command.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace tisple {

namespace {

struct DiskBuildArgs {
    const char* path = nullptr;
    std::string disk;   // the image to write ("" for run: kept in memory)
    std::string binary; // -o: also write the .bin
    BuildOptions options;
    bool headless = false;
    std::vector<char*> machine_args;
};

bool ParseNumber(const char* text, uint64_t& out) {
    char* end = nullptr;
    if (text[0] == '$') {
        out = std::strtoull(text + 1, &end, 16);
        return text[1] != '\0' && *end == '\0';
    }
    out = std::strtoull(text, &end, 0);
    return text[0] != '\0' && *end == '\0';
}

bool IsImageName(const char* arg) {
    return DiskFormatForPath(arg) != DiskFormat::kNone;
}

// machine flags that mean "no window", as for plain tisple run
bool IsHeadlessFlag(const char* arg) {
    const char* flags[] = {"--cycles", "--frames", "--screenshot", "--wav", "--save-snapshot"};
    for (const char* flag : flags) {
        if (std::strcmp(arg, flag) == 0) {
            return true;
        }
    }
    return false;
}

bool ParseArgs(int argc, char** argv, bool run, DiskBuildArgs& args) {
    // no DOS to keep clear of: the loader can fill memory up to $BFFF
    args.options.limit = 0xC000;
    for (int i = 2; i < argc; i++) {
        const char* arg = argv[i];
        bool has_value = i + 1 < argc;
        uint64_t number = 0;
        if (std::strcmp(arg, "--disk") == 0) {
            if (has_value && IsImageName(argv[i + 1])) {
                args.disk = argv[++i];
            } else if (!run) {
                return false;
            }
        } else if (std::strcmp(arg, "--origin") == 0 && has_value) {
            if (!ParseNumber(argv[++i], number) || number < 0x0A00 || number > 0xBFFF) {
                return false;
            }
            args.options.origin = static_cast<uint16_t>(number);
        } else if (!run && std::strcmp(arg, "-o") == 0 && has_value) {
            args.binary = argv[++i];
        } else if (run && std::strcmp(arg, "--headless") == 0) {
            args.headless = true;
        } else if (args.path == nullptr && arg[0] != '-') {
            args.path = arg;
        } else if (run && args.path != nullptr) {
            args.headless = args.headless || IsHeadlessFlag(arg);
            args.machine_args.push_back(argv[i]);
        } else {
            return false;
        }
    }
    return args.path != nullptr && (run || !args.disk.empty());
}

bool CompileFor(const DiskBuildArgs& args, Compiled& compiled) {
    Program program;
    std::vector<Diagnostic> errors;
    if (!BuildProgram(args.path, LoadOptions(), program, errors) ||
        !CompileProgram(program, args.options, compiled, errors)) {
        std::fputs(FormatDiagnostics(program.sources, errors).c_str(), stderr);
        return false;
    }
    return true;
}

} // namespace

bool HasDiskFlag(int argc, char** argv) {
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--disk") == 0) {
            return true;
        }
    }
    return false;
}

int RunBuildDiskCommand(int argc, char** argv) {
    DiskBuildArgs args;
    if (!ParseArgs(argc, argv, false, args)) {
        std::fprintf(stderr,
                     "usage: tisple build main.tsp --disk out.dsk [--origin A] [-o out.bin]\n"
                     "a disk that boots the program on its own (no DOS); the program\n"
                     "loads in $0A00-$BFFF\n");
        return 1;
    }
    Compiled compiled;
    if (!CompileFor(args, compiled)) {
        return 1;
    }
    if (!args.binary.empty() && !WriteWholeFile(args.binary, compiled.image)) {
        std::fprintf(stderr, "tisple: cannot write %s\n", args.binary.c_str());
        return 1;
    }
    BootDiskInfo info;
    std::string error;
    if (!WriteBootDisk(args.disk, compiled.image, compiled.origin, info, error)) {
        std::fprintf(stderr, "tisple: %s\n", error.c_str());
        return 1;
    }
    return 0;
}

int RunRunDiskCommand(int argc, char** argv, MachineWindowFn window) {
    DiskBuildArgs args;
    if (!ParseArgs(argc, argv, true, args)) {
        std::fprintf(stderr,
                     "usage: tisple run main.tsp --disk [out.dsk] [--origin A] [machine flags]\n"
                     "builds a self-booting disk and boots it (our boot rom, or\n"
                     "--rom Apple2e.rom [--disk-rom DISK2.rom]); see tisple machine\n");
        return 1;
    }
    Compiled compiled;
    if (!CompileFor(args, compiled)) {
        return 1;
    }
    if (compiled.main < 0) {
        std::fprintf(stderr, "%s: no fn main to run; add (fn main () void ...)\n", args.path);
        return 1;
    }
    std::unique_ptr<DiskSectors> disk = std::make_unique<DiskSectors>();
    BootDiskInfo info;
    std::string error;
    if (!MakeBootDisk(compiled.image, compiled.origin, *disk, info, error)) {
        std::fprintf(stderr, "tisple: %s\n", error.c_str());
        return 1;
    }
    if (!args.disk.empty() && !WriteDiskSectorsFile(args.disk, *disk, error)) {
        std::fprintf(stderr, "tisple: %s\n", error.c_str());
        return 1;
    }
    std::printf("%zu bytes at $%04X on a boot disk (%s)\n", compiled.image.size(), compiled.origin,
                BootTracksText(info).c_str());
    static char window_flag[] = "--window";
    if (!args.headless && window != nullptr) {
        args.machine_args.push_back(window_flag);
    }
    std::vector<uint8_t> image = ImageFromSectors(*disk, SectorOrder::kDos);
    return RunMachineOnDisk(image, DiskFormat::kDos, static_cast<int>(args.machine_args.size()),
                            args.machine_args.data(), window);
}

} // namespace tisple
