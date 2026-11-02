// tisple build main.tsp --disk out.dsk / tisple run main.tsp --disk: compile, then a
// self-booting disk; run boots it on the built-in machine as a rehearsal of real hardware

#include "backend/backend.hpp"
#include "backend/build_command.hpp"
#include "check/check.hpp"
#include "check/diag.hpp"
#include "disk/disk_command.hpp"
#include "disk/disk_tables.hpp"
#include "disk/image.hpp"
#include "disk/vars_export.hpp"
#include "machine/machine_command.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tisple {

namespace {

// the disk's folder, made when missing (a project's build/), as other build tools do
void MakeDiskFolder(const std::string& path) {
    std::filesystem::path folder = std::filesystem::path(path).parent_path();
    std::error_code ec;
    if (!folder.empty()) {
        std::filesystem::create_directories(folder, ec);
    }
}

} // namespace

namespace {

struct DiskBuildArgs {
    const char* path = nullptr;
    std::string disk;   // the image to write ("" for run: kept in memory)
    std::string disk2;  // --disk2: the second disk's image ("" for disk's name with a 2)
    std::string binary; // -o: also write the .bin
    BuildOptions options;
    bool listing = false; // build: --asm
    bool tables = false;  // build: --tables, each table :on disk or disk2 and where it went
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
    UseBootDiskLayout(args.options);
    for (int i = 2; i < argc; i++) {
        const char* arg = argv[i];
        bool has_value = i + 1 < argc;
        std::string flag_error;
        if (ParseCodeFlag(argc, argv, i, args.options, flag_error)) {
            if (!flag_error.empty()) {
                std::fprintf(stderr, "tisple: %s\n", flag_error.c_str());
                return false;
            }
            continue;
        }
        uint64_t number = 0;
        if (std::strcmp(arg, "--disk") == 0) {
            if (has_value && IsImageName(argv[i + 1])) {
                args.disk = argv[++i];
            } else if (!run) {
                return false;
            }
        } else if (std::strcmp(arg, "--disk2") == 0 && has_value && IsImageName(argv[i + 1])) {
            args.disk2 = argv[++i];
        } else if (std::strcmp(arg, "--origin") == 0 && has_value) {
            if (!ParseNumber(argv[++i], number) || number < kBootLoadLow || number > 0xBFFF) {
                return false;
            }
            args.options.origin = static_cast<uint16_t>(number);
        } else if (!run && std::strcmp(arg, "-o") == 0 && has_value) {
            args.binary = argv[++i];
        } else if (!run && std::strcmp(arg, "--asm") == 0) {
            args.listing = true;
        } else if (!run && std::strcmp(arg, "--tables") == 0) {
            args.tables = true;
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

bool CompileFor(const DiskBuildArgs& args, Program& program, Compiled& compiled) {
    std::vector<Diagnostic> errors;
    if (!BuildProgram(args.path, LoadOptions(), program, errors) ||
        !CompileProgram(program, args.options, compiled, errors)) {
        std::fputs(FormatDiagnostics(program.sources, errors).c_str(), stderr);
        return false;
    }
    if (args.options.opt.bisect >= 0) {
        std::fputs(DescribeOptReport(compiled.opt).c_str(), stderr);
    }
    std::fputs(FormatDiagnostics(program.sources, compiled.warnings).c_str(), stderr);
    if (args.listing) {
        std::fputs(ListProgram(program, compiled).c_str(), stdout);
    }
    return true;
}

// the second disk's image: --disk2's, else the first disk's name with a 2 before its extension
// (build/game.dsk, build/game2.dsk); "" when run keeps the disk in memory
std::string Disk2Path(const DiskBuildArgs& args) {
    if (!args.disk2.empty() || args.disk.empty()) {
        return args.disk2;
    }
    std::filesystem::path path(args.disk);
    std::filesystem::path name = path.stem();
    name += "2";
    name += path.extension();
    return (path.parent_path() / name).string();
}

// every table :on disk and :on disk2: its name, disk, bytes, sectors and tracks
std::string DescribeDiskTables(const Program& program, const Compiled& compiled) {
    std::string out;
    for (const DiskTable& t : program.disk_tables) {
        char line[256];
        int first = t.tracks.empty() ? 0 : t.tracks.front();
        int last = t.tracks.empty() ? 0 : t.tracks.back();
        std::snprintf(line, sizeof(line), "  disk %d tracks %2d-%2d: %s, %zu bytes, %u sectors\n",
                      t.disk, first, last,
                      program.vars[static_cast<std::size_t>(t.var)].name.c_str(), t.bytes.size(),
                      DiskTableSectors(static_cast<std::uint32_t>(t.bytes.size())));
        out += line;
    }
    for (std::size_t g = 0; g < compiled.overlays.groups.size(); g++) {
        const OverlayImage& image = compiled.overlays.groups[g];
        if (!image.on_disk) {
            continue;
        }
        const DiskTable& t =
            compiled.overlays.disk_tables[static_cast<std::size_t>(image.disk_table)];
        char line[256];
        std::snprintf(line, sizeof(line),
                      "  disk 2 tracks %2d-%2d: overlay group %zu's code, %zu bytes, %u sectors\n",
                      t.tracks.front(), t.tracks.back(), g, t.bytes.size(),
                      DiskTableSectors(static_cast<std::uint32_t>(t.bytes.size())));
        out += line;
    }
    return out;
}

// the second disk, when the program has tables :on disk2: written to path (unless ""), and
// its image kept in image (unless null)
bool WriteDisk2(const Program& program, const Compiled& compiled, const std::string& path,
                std::vector<uint8_t>* image, std::string& error) {
    if (!HasDisk2Tables(program.disk_tables)) {
        return true;
    }
    // the program's tables, then the overlay groups' images read from there
    std::vector<DiskTable> tables = program.disk_tables;
    tables.insert(tables.end(), compiled.overlays.disk_tables.begin(),
                  compiled.overlays.disk_tables.end());
    std::unique_ptr<DiskSectors> disk = std::make_unique<DiskSectors>();
    MakeDisk2(tables, *disk);
    if (image != nullptr) {
        *image = ImageFromSectors(*disk, SectorOrder::kDos);
    }
    if (path.empty()) {
        return true;
    }
    MakeDiskFolder(path);
    if (!WriteDiskSectorsFile(path, *disk, error)) {
        return false;
    }
    std::size_t count = 0;
    for (const DiskTable& t : program.disk_tables) {
        count += t.disk == 2 ? 1 : 0;
    }
    int last = Disk2LastTrack(tables);
    std::string images;
    if (!compiled.overlays.disk_tables.empty()) {
        images = " and " + std::to_string(compiled.overlays.disk_tables.size()) +
                 " overlay groups' code";
    }
    std::printf("%s: the second disk, %zu tables :on disk2%s in tracks %d-%d (%d free)\n",
                path.c_str(), count, images.c_str(), kDisk2FirstTrack, last, kBootLastTrack - last);
    return true;
}

// game.vars.json beside game.dsk (disk/vars_export.hpp): the globals' addresses for the phone
// player, with the hash of the disk as written so a stale one is known
bool WriteVarsJson(const Program& program, const Compiled& compiled, const std::string& disk,
                   std::string& error) {
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(disk, bytes)) {
        error = "cannot read " + disk + " back";
        return false;
    }
    std::string text = DescribeVarsJson(program, compiled, Fnv1a32(bytes));
    std::string path = VarsJsonPath(disk);
    if (!WriteWholeFile(path, std::vector<uint8_t>(text.begin(), text.end()))) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

} // namespace

// what the program adds to its disk: its tables :on disk, lib/disk's reader where the library
// keeps room for it (disk.reader, and disk.work for its tables), and lib/save's writer
// (save.writer)
BootExtras BootExtrasFor(const Program& program) {
    BootExtras extras;
    extras.tables = &program.disk_tables;
    std::int32_t reader = FindVar(program, "disk.reader");
    if (reader >= 0) {
        const Var& var = program.vars[static_cast<std::size_t>(reader)];
        extras.reader = var.placement.address;
        extras.reader_room = TypeOf(program, var.type).size;
    }
    std::int32_t writer = FindVar(program, "save.writer");
    if (writer >= 0) {
        const Var& var = program.vars[static_cast<std::size_t>(writer)];
        extras.writer = var.placement.address;
        extras.writer_room = TypeOf(program, var.type).size;
    }
    std::int32_t work = FindVar(program, "disk.work");
    if (work >= 0) {
        const Var& var = program.vars[static_cast<std::size_t>(work)];
        extras.work = var.placement.address;
        extras.work_room = TypeOf(program, var.type).size;
    }
    return extras;
}

void UseBootDiskLayout(BuildOptions& options) {
    // no DOS to keep clear of: the loader can fill memory up to $BFFF, and from $0B00 below
    // the origin
    options.limit = 0xC000;
    options.low_start = kBootLoadLow;
    options.homes_in_segments = true;
    // lib/disk's reader is on a boot disk: groups marked :overlay_disk2 are read from disk 2
    options.disk2_images = true;
}

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
        std::fprintf(
            stderr,
            "usage: tisple build main.tsp --disk out.dsk [--disk2 out2.dsk] [--origin A]\n"
            "  [-o out.bin] [--asm] [--tables] [-O0|-O1] [--opt-bisect N] [--no-pass NAME]\n"
            "  [--no-inline module.fn]\n"
            "a disk that boots the program on its own (no DOS); the program\n"
            "loads in $0B00-$BFFF, the language card and aux memory\n"
            "a tisple.project at the project root lets tisple build run with no file\n");
        return 1;
    }
    Program program;
    Compiled compiled;
    if (!CompileFor(args, program, compiled)) {
        return 1;
    }
    if (!args.binary.empty() && !compiled.segments.empty()) {
        std::fprintf(stderr, "tisple: the program does not fit one file, so -o has nothing to "
                             "write; the disk holds every part\n");
        return 1;
    }
    if (!args.binary.empty() && !WriteWholeFile(args.binary, compiled.image)) {
        std::fprintf(stderr, "tisple: cannot write %s\n", args.binary.c_str());
        return 1;
    }
    std::fputs(DescribeSegments(compiled).c_str(), stdout);
    if (std::getenv("TISPLE_DATA_NOTES") != nullptr) {
        for (std::size_t v = 0; v < program.vars.size(); v++) {
            const VarPlace& place = compiled.layout.vars[v];
            const Var& var = program.vars[v];
            if (var.function >= 0 || place.size == 0 || place.dropped || place.interp_only) {
                continue;
            }
            std::printf("data %5u $%04X %s %s\n", place.size, place.address,
                        var.placement.in_aux ? "aux " : "main", var.name.c_str());
        }
    }
    if (args.tables) {
        std::fputs(DescribeDiskTables(program, compiled).c_str(), stdout);
    }
    BootDiskInfo info;
    std::string error;
    MakeDiskFolder(args.disk);
    if (!WriteBootDisk(args.disk, ProgramSegments(compiled), compiled.origin, info, error,
                       BootExtrasFor(program)) ||
        !WriteDisk2(program, compiled, Disk2Path(args), nullptr, error) ||
        !WriteVarsJson(program, compiled, args.disk, error)) {
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
                     "--rom Apple2e.rom [--disk-rom DISK2.rom]); see tisple machine\n"
                     "a tisple.project at the project root lets tisple run run with no file\n");
        return 1;
    }
    Program program;
    Compiled compiled;
    if (!CompileFor(args, program, compiled)) {
        return 1;
    }
    if (compiled.main < 0) {
        std::fprintf(stderr, "%s: no fn main to run; add (fn main () void ...)\n", args.path);
        return 1;
    }
    std::unique_ptr<DiskSectors> disk = std::make_unique<DiskSectors>();
    BootDiskInfo info;
    std::string error;
    if (!MakeBootDisk(ProgramSegments(compiled), compiled.origin, *disk, info, error,
                      BootExtrasFor(program))) {
        std::fprintf(stderr, "tisple: %s\n", error.c_str());
        return 1;
    }
    if (!args.disk.empty()) {
        MakeDiskFolder(args.disk);
    }
    if (!args.disk.empty() && (!WriteDiskSectorsFile(args.disk, *disk, error) ||
                               !WriteVarsJson(program, compiled, args.disk, error))) {
        std::fprintf(stderr, "tisple: %s\n", error.c_str());
        return 1;
    }
    std::vector<uint8_t> image2;
    std::string path2 = Disk2Path(args);
    if (!WriteDisk2(program, compiled, path2, &image2, error)) {
        std::fprintf(stderr, "tisple: %s\n", error.c_str());
        return 1;
    }
    std::fputs(DescribeSegments(compiled).c_str(), stdout);
    std::printf("on a boot disk (%s)\n", BootTracksText(info).c_str());
    static char window_flag[] = "--window";
    if (!args.headless && window != nullptr) {
        args.machine_args.push_back(window_flag);
    }
    // the disk from its file, so --write-back saves what the program wrote (lib/save) there
    static char disk_flag[] = "--disk";
    if (!args.disk.empty()) {
        args.machine_args.push_back(disk_flag);
        args.machine_args.push_back(const_cast<char*>(args.disk.c_str()));
    }
    static char disk2_flag[] = "--disk2";
    if (!path2.empty() && !image2.empty()) {
        args.machine_args.push_back(disk2_flag);
        args.machine_args.push_back(const_cast<char*>(path2.c_str()));
    }
    std::vector<uint8_t> image = ImageFromSectors(*disk, SectorOrder::kDos);
    return RunMachineOnDisk(image, DiskFormat::kDos, static_cast<int>(args.machine_args.size()),
                            args.machine_args.data(), window, compiled.cpu, image2);
}

} // namespace tisple
