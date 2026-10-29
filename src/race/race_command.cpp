#include "race/race_command.hpp"

#include "race/race.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace tisple {

namespace {

struct RaceArgs {
    std::vector<RacePatch> loads;
    RaceOptions options;
    bool has_start = false;
    bool has_entry = false;
    std::string dump;                 // memory after the kernel, all 64 KB
    std::string executed;             // ranges of the code that ran
    std::string profile;              // runs and cycles of each instruction on the clock
    std::vector<std::uint16_t> words; // u16s to print from memory after the kernel
};

void PrintRaceUsage() {
    std::fprintf(stderr,
                 "usage: tisple race --load FILE@ADDR... --start ADDR --entry ADDR\n"
                 "                   [--patch FILE@ADDR]... [--dump mem.bin] [--executed out.txt]\n"
                 "                   [--max-cycles N] [--after ADDR] [--stop ADDR]\n"
                 "                   [--word ADDR]... [--profile out.txt]\n"
                 "runs a binary from --start; when the pc first reaches --entry, writes the\n"
                 "patches (the kernel's inputs) and counts cycles until it returns. prints the\n"
                 "kernel's cycles and registers; --dump writes memory after it, --executed the\n"
                 "address ranges of every instruction it ran. --after: the clock starts when the\n"
                 "kernel's call of the function at ADDR returns; --stop: it stops when the pc\n"
                 "first reaches ADDR, and the kernel runs on to its return. --word prints the\n"
                 "u16 at ADDR after it; --profile writes \"pc runs cycles\" for each instruction\n"
                 "that ran on the clock. addresses take $hex, 0xhex or decimal\n");
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

// a FILE@ADDR flag's value; prints why when it cannot be read
bool ReadFileArg(const char* value, std::vector<RacePatch>& out) {
    RacePatch patch;
    std::string error;
    if (!ReadPatchArg(value, patch, error)) {
        std::fprintf(stderr, "tisple race: %s\n", error.c_str());
        return false;
    }
    out.push_back(patch);
    return true;
}

bool ParseOne(int argc, char** argv, int& i, RaceArgs& args) {
    const char* arg = argv[i];
    if (i + 1 >= argc) {
        return false;
    }
    const char* value = argv[++i];
    std::uint64_t number = 0;
    if (std::strcmp(arg, "--load") == 0) {
        return ReadFileArg(value, args.loads);
    }
    if (std::strcmp(arg, "--patch") == 0) {
        return ReadFileArg(value, args.options.patches);
    }
    if (std::strcmp(arg, "--start") == 0) {
        args.has_start = true;
        return ParseAddress(value, args.options.start);
    }
    if (std::strcmp(arg, "--entry") == 0) {
        args.has_entry = true;
        return ParseAddress(value, args.options.entry);
    }
    if (std::strcmp(arg, "--word") == 0) {
        std::uint16_t address = 0;
        if (!ParseAddress(value, address)) {
            return false;
        }
        args.words.push_back(address);
        return true;
    }
    if (std::strcmp(arg, "--after") == 0) {
        args.options.has_after = true;
        return ParseAddress(value, args.options.after);
    }
    if (std::strcmp(arg, "--stop") == 0) {
        args.options.has_stop = true;
        return ParseAddress(value, args.options.stop);
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
    if (std::strcmp(arg, "--profile") == 0) {
        args.profile = value;
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

bool WriteFile(const std::string& path, const void* data, std::size_t size) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    bool ok = std::fwrite(data, 1, size, file) == size;
    return std::fclose(file) == 0 && ok;
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
    if (!args.profile.empty()) {
        std::string lines = ProfileLines(result);
        if (!WriteFile(args.profile, lines.data(), lines.size())) {
            std::fprintf(stderr, "tisple race: cannot write %s\n", args.profile.c_str());
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
    for (const RacePatch& load : args.loads) {
        LoadBytes(*machine, load.address, load.bytes.data(), load.bytes.size());
    }

    RaceResult result = RunRace(*machine, args.options);
    if (!WriteOutputs(args, *machine, result)) {
        return 1;
    }
    std::printf("startup_cycles %llu\n", static_cast<unsigned long long>(result.startup_cycles));
    std::printf("kernel_cycles %llu\n", static_cast<unsigned long long>(result.kernel_cycles));
    std::printf("total_cycles %llu\n", static_cast<unsigned long long>(result.total_cycles));
    std::printf("regs A=%02X X=%02X Y=%02X S=%02X P=%02X\n", result.regs.a, result.regs.x,
                result.regs.y, result.regs.s, result.regs.p);
    for (std::uint16_t address : args.words) {
        unsigned value =
            machine->mem[address] | (machine->mem[static_cast<std::uint16_t>(address + 1)] << 8);
        std::printf("word $%04X %u\n", address, value);
    }
    if (!result.returned) {
        std::printf("error %s\n", result.error.c_str());
        return 2;
    }
    std::printf("returned\n");
    return 0;
}

} // namespace tisple
