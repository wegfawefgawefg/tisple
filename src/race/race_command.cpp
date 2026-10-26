#include "race/race_command.hpp"

#include "race/race.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace tisple {

namespace {

struct FileAt {
    std::string path;
    std::uint16_t address = 0;
};

struct RaceArgs {
    std::vector<FileAt> loads;
    std::vector<FileAt> patches;
    RaceOptions options;
    bool has_start = false;
    bool has_entry = false;
    std::string dump;     // memory after the kernel, all 64 KB
    std::string executed; // ranges of the code that ran
};

void PrintRaceUsage() {
    std::fprintf(stderr,
                 "usage: tisple race --load FILE@ADDR... --start ADDR --entry ADDR\n"
                 "                   [--patch FILE@ADDR]... [--dump mem.bin] [--executed out.txt]\n"
                 "                   [--max-cycles N]\n"
                 "runs a binary from --start; when the pc first reaches --entry, writes the\n"
                 "patches (the kernel's inputs) and counts cycles until it returns. prints the\n"
                 "kernel's cycles and registers; --dump writes memory after it, --executed the\n"
                 "address ranges of every instruction it ran. addresses take $hex, 0xhex or\n"
                 "decimal\n");
}

bool ParseNumber(const char* text, std::uint64_t& out) {
    int base = 0;
    if (text[0] == '$') {
        text++;
        base = 16;
    }
    char* end = nullptr;
    out = std::strtoull(text, &end, base);
    return *text != '\0' && *end == '\0';
}

bool ParseAddress(const char* text, std::uint16_t& out) {
    std::uint64_t number = 0;
    if (!ParseNumber(text, number) || number > 0xFFFF) {
        return false;
    }
    out = static_cast<std::uint16_t>(number);
    return true;
}

// FILE@ADDR, split at the last @
bool ParseFileAt(const char* text, FileAt& out) {
    const char* at = std::strrchr(text, '@');
    if (at == nullptr || at == text) {
        return false;
    }
    out.path.assign(text, static_cast<std::size_t>(at - text));
    return ParseAddress(at + 1, out.address);
}

bool ParseOne(int argc, char** argv, int& i, RaceArgs& args) {
    const char* arg = argv[i];
    if (i + 1 >= argc) {
        return false;
    }
    const char* value = argv[++i];
    FileAt file;
    std::uint64_t number = 0;
    if (std::strcmp(arg, "--load") == 0 && ParseFileAt(value, file)) {
        args.loads.push_back(file);
        return true;
    }
    if (std::strcmp(arg, "--patch") == 0 && ParseFileAt(value, file)) {
        args.patches.push_back(file);
        return true;
    }
    if (std::strcmp(arg, "--start") == 0) {
        args.has_start = true;
        return ParseAddress(value, args.options.start);
    }
    if (std::strcmp(arg, "--entry") == 0) {
        args.has_entry = true;
        return ParseAddress(value, args.options.entry);
    }
    if (std::strcmp(arg, "--max-cycles") == 0 && ParseNumber(value, number)) {
        args.options.max_cycles = number;
        return true;
    }
    if (std::strcmp(arg, "--dump") == 0) {
        args.dump = value;
        return true;
    }
    if (std::strcmp(arg, "--executed") == 0) {
        args.executed = value;
        return true;
    }
    return false;
}

bool ParseArgs(int argc, char** argv, RaceArgs& args) {
    for (int i = 2; i < argc; i++) {
        if (!ParseOne(argc, argv, i, args)) {
            return false;
        }
    }
    return !args.loads.empty() && args.has_start && args.has_entry;
}

bool ReadFile(const std::string& path, std::vector<std::uint8_t>& bytes) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    std::uint8_t chunk[4096];
    std::size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + got);
    }
    std::fclose(file);
    return true;
}

bool WriteFile(const std::string& path, const void* data, std::size_t size) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    bool ok = std::fwrite(data, 1, size, file) == size;
    return std::fclose(file) == 0 && ok;
}

// reads a FILE@ADDR that must fit below $10000
bool ReadFileAt(const FileAt& file, std::vector<std::uint8_t>& bytes) {
    if (!ReadFile(file.path, bytes)) {
        std::fprintf(stderr, "tisple race: cannot read %s\n", file.path.c_str());
        return false;
    }
    if (file.address + bytes.size() > 0x10000) {
        std::fprintf(stderr, "tisple race: %s does not fit at $%04X\n", file.path.c_str(),
                     file.address);
        return false;
    }
    return true;
}

bool WriteOutputs(const RaceArgs& args, const Machine& m, const RaceResult& result) {
    if (!args.dump.empty() && !WriteFile(args.dump, m.mem, sizeof(m.mem))) {
        std::fprintf(stderr, "tisple race: cannot write %s\n", args.dump.c_str());
        return false;
    }
    if (!args.executed.empty()) {
        std::string ranges = ExecutedRanges(m, result.executed);
        if (!WriteFile(args.executed, ranges.data(), ranges.size())) {
            std::fprintf(stderr, "tisple race: cannot write %s\n", args.executed.c_str());
            return false;
        }
    }
    return true;
}

} // namespace

int RunRaceCommand(int argc, char** argv) {
    RaceArgs args;
    if (!ParseArgs(argc, argv, args)) {
        PrintRaceUsage();
        return 1;
    }
    auto machine = std::make_unique<Machine>();
    InitMachine(*machine);
    for (const FileAt& load : args.loads) {
        std::vector<std::uint8_t> bytes;
        if (!ReadFileAt(load, bytes)) {
            return 1;
        }
        LoadBytes(*machine, load.address, bytes.data(), bytes.size());
    }
    for (const FileAt& patch_file : args.patches) {
        RacePatch patch;
        patch.address = patch_file.address;
        if (!ReadFileAt(patch_file, patch.bytes)) {
            return 1;
        }
        args.options.patches.push_back(patch);
    }

    RaceResult result = RunRace(*machine, args.options);
    if (!WriteOutputs(args, *machine, result)) {
        return 1;
    }
    std::printf("startup_cycles %llu\n", static_cast<unsigned long long>(result.startup_cycles));
    std::printf("kernel_cycles %llu\n", static_cast<unsigned long long>(result.kernel_cycles));
    std::printf("regs A=%02X X=%02X Y=%02X S=%02X P=%02X\n", result.regs.a, result.regs.x,
                result.regs.y, result.regs.s, result.regs.p);
    if (!result.returned) {
        std::printf("error %s\n", result.error.c_str());
        return 2;
    }
    std::printf("returned\n");
    return 0;
}

} // namespace tisple
