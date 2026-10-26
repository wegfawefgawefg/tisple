#include "emu/sim_command.hpp"

#include "emu/disasm.hpp"
#include "emu/harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace tisple {

namespace {

struct SimArgs {
    const char* path = nullptr;
    uint32_t load = 0x0803;
    uint32_t start = 0x0803;
    int32_t stop = -1;
    uint64_t max_cycles = 100000000;
    bool trace = false;
};

void PrintSimUsage() {
    std::fprintf(stderr, "usage: tisple sim <file.bin> [--load ADDR] [--start ADDR] [--stop ADDR]\n"
                         "                  [--max-cycles N] [--trace]\n"
                         "addresses take $hex, 0xhex or decimal\n");
}

bool ParseNumber(const char* text, uint64_t& out) {
    if (text[0] == '$') {
        text++;
        char* end = nullptr;
        out = std::strtoull(text, &end, 16);
        return *text != '\0' && *end == '\0';
    }
    char* end = nullptr;
    out = std::strtoull(text, &end, 0);
    return *text != '\0' && *end == '\0';
}

bool ParseArgs(int argc, char** argv, SimArgs& args) {
    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];
        bool has_value = i + 1 < argc;
        uint64_t number = 0;
        if (std::strcmp(arg, "--trace") == 0) {
            args.trace = true;
        } else if (std::strcmp(arg, "--load") == 0 && has_value) {
            if (!ParseNumber(argv[++i], number) || number > 0xFFFF) {
                return false;
            }
            args.load = static_cast<uint32_t>(number);
        } else if (std::strcmp(arg, "--start") == 0 && has_value) {
            if (!ParseNumber(argv[++i], number) || number > 0xFFFF) {
                return false;
            }
            args.start = static_cast<uint32_t>(number);
        } else if (std::strcmp(arg, "--stop") == 0 && has_value) {
            if (!ParseNumber(argv[++i], number) || number > 0xFFFF) {
                return false;
            }
            args.stop = static_cast<int32_t>(number);
        } else if (std::strcmp(arg, "--max-cycles") == 0 && has_value) {
            if (!ParseNumber(argv[++i], number)) {
                return false;
            }
            args.max_cycles = number;
        } else if (arg[0] != '-' && args.path == nullptr) {
            args.path = arg;
        } else {
            return false;
        }
    }
    return args.path != nullptr;
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

// "nv-bdizc" with capitals for set flags
void FlagString(uint8_t p, char* out) {
    const char* names = "NV-BDIZC";
    for (int i = 0; i < 8; i++) {
        bool set = (p & (0x80 >> i)) != 0;
        out[i] = set ? names[i] : static_cast<char>(names[i] | 0x20);
    }
    out[8] = '\0';
}

void PrintTraceLine(void* ctx, const Machine& m) {
    (void)ctx;
    Regs r = GetRegs(m);
    int length = 0;
    std::string text = Disassemble(m, r.pc, length);
    char bytes[16] = "";
    for (int i = 0; i < length && i < 3; i++) {
        std::snprintf(bytes + i * 3, 4, "%02X ", m.mem[static_cast<uint16_t>(r.pc + i)]);
    }
    char flags[9];
    FlagString(r.p, flags);
    std::printf("%04X  %-9s %-12s A=%02X X=%02X Y=%02X S=%02X P=%02X %s cyc=%llu\n", r.pc, bytes,
                text.c_str(), r.a, r.x, r.y, r.s, r.p, flags,
                static_cast<unsigned long long>(m.cycles));
}

} // namespace

int RunSimCommand(int argc, char** argv) {
    SimArgs args;
    if (!ParseArgs(argc, argv, args)) {
        PrintSimUsage();
        return 1;
    }
    std::vector<uint8_t> bytes;
    if (!ReadFile(args.path, bytes)) {
        std::fprintf(stderr, "tisple sim: cannot read '%s'\n", args.path);
        return 1;
    }
    if (args.load + bytes.size() > 0x10000) {
        std::fprintf(stderr, "tisple sim: binary does not fit in memory at $%04X\n", args.load);
        return 1;
    }

    auto machine = std::make_unique<Machine>();
    InitMachine(*machine);
    LoadBytes(*machine, static_cast<uint16_t>(args.load), bytes.data(), bytes.size());
    Regs regs;
    regs.pc = static_cast<uint16_t>(args.start);
    SetRegs(*machine, regs);

    RunOptions options;
    options.stop_pc = args.stop;
    options.max_cycles = args.max_cycles;
    if (args.trace) {
        options.trace = PrintTraceLine;
    }
    RunResult result = Run(*machine, options);

    char flags[9];
    FlagString(result.regs.p, flags);
    std::printf("stopped: %s at $%04X\n", StopReasonName(result.reason), result.regs.pc);
    std::printf("A=%02X X=%02X Y=%02X S=%02X P=%02X %s\n", result.regs.a, result.regs.x,
                result.regs.y, result.regs.s, result.regs.p, flags);
    std::printf("cycles: %llu\n", static_cast<unsigned long long>(result.cycles));
    return 0;
}

} // namespace tisple
