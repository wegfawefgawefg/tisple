#include "race/race.hpp"

#include "emu/disasm.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tisple {

namespace {

// why the instruction at pc cannot run, or nullptr
const char* BadInstruction(const Machine& m, std::uint16_t pc) {
    std::uint8_t opcode = m.mem[pc];
    if (opcode == 0x00) {
        return "BRK";
    }
    if (!IsDocumentedOpcode(opcode)) {
        return "undocumented opcode";
    }
    return nullptr;
}

std::string ErrorAt(const char* what, const char* phase, std::uint16_t pc) {
    char text[96];
    std::snprintf(text, sizeof(text), "%s at $%04X %s", what, pc, phase);
    return text;
}

// runs until the pc reaches entry; false with the error set if something else happens first
bool RunToEntry(Machine& m, const RaceOptions& options, RaceResult& result) {
    std::uint64_t start_cycles = m.cycles;
    for (;;) {
        std::uint16_t pc = M6502_GET_ADDR(m.pins);
        if (pc == options.entry) {
            result.startup_cycles = m.cycles - start_cycles;
            return true;
        }
        const char* bad = BadInstruction(m, pc);
        if (bad != nullptr) {
            result.error = ErrorAt(bad, "before the kernel started", pc);
            return false;
        }
        if (m.cycles - start_cycles >= options.max_startup) {
            result.error = "the kernel was never entered (startup cycle limit)";
            return false;
        }
        StepInstruction(m);
    }
}

void ApplyPatches(Machine& m, const RaceOptions& options) {
    for (const RacePatch& patch : options.patches) {
        LoadBytes(m, patch.address, patch.bytes.data(), patch.bytes.size());
    }
}

bool ParseAddress(const char* text, std::uint16_t& out) {
    int base = 0;
    if (text[0] == '$') {
        text++;
        base = 16;
    }
    char* end = nullptr;
    unsigned long long number = std::strtoull(text, &end, base);
    if (*text == '\0' || *end != '\0' || number > 0xFFFF) {
        return false;
    }
    out = static_cast<std::uint16_t>(number);
    return true;
}

} // namespace

bool ReadPatchArg(const char* text, RacePatch& out, std::string& error) {
    const char* at = std::strrchr(text, '@');
    if (at == nullptr || at == text || !ParseAddress(at + 1, out.address)) {
        error = std::string("expected FILE@ADDR, got ") + text;
        return false;
    }
    std::string path(text, static_cast<std::size_t>(at - text));
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = "cannot read " + path;
        return false;
    }
    out.bytes.clear();
    std::uint8_t chunk[4096];
    std::size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        out.bytes.insert(out.bytes.end(), chunk, chunk + got);
    }
    std::fclose(file);
    if (out.address + out.bytes.size() > 0x10000) {
        error = path + " does not fit at " + (at + 1);
        return false;
    }
    return true;
}

RaceResult RunRace(Machine& m, const RaceOptions& options) {
    RaceResult result;
    result.executed.assign(65536, 0);
    Regs regs = GetRegs(m);
    regs.pc = options.start;
    SetRegs(m, regs);
    if (!RunToEntry(m, options, result)) {
        return result;
    }
    ApplyPatches(m, options);

    // the caller's return address is on top of the stack: JSR pushed it minus one
    std::uint8_t sp = m.cpu.S;
    std::uint16_t low = m.mem[0x100 + static_cast<std::uint8_t>(sp + 1)];
    std::uint16_t high = m.mem[0x100 + static_cast<std::uint8_t>(sp + 2)];
    std::uint16_t back = static_cast<std::uint16_t>((low | (high << 8)) + 1);
    std::uint8_t back_sp = static_cast<std::uint8_t>(sp + 2);

    std::uint64_t start_cycles = m.cycles;
    for (;;) {
        std::uint16_t pc = M6502_GET_ADDR(m.pins);
        if (pc == back && m.cpu.S == back_sp) {
            result.returned = true;
            break;
        }
        const char* bad = BadInstruction(m, pc);
        if (bad != nullptr) {
            result.error = ErrorAt(bad, "inside the kernel", pc);
            break;
        }
        if (m.cycles - start_cycles >= options.max_cycles) {
            result.error = "kernel cycle limit";
            break;
        }
        result.executed[pc] = 1;
        StepInstruction(m);
    }
    result.kernel_cycles = m.cycles - start_cycles;
    result.regs = GetRegs(m);
    return result;
}

std::string ExecutedRanges(const Machine& m, const std::vector<std::uint8_t>& executed) {
    std::vector<std::uint8_t> bytes(65536, 0);
    for (std::uint32_t pc = 0; pc < 65536 && pc < executed.size(); pc++) {
        if (executed[pc] == 0) {
            continue;
        }
        int length = 1;
        Disassemble(m, static_cast<std::uint16_t>(pc), length);
        for (int i = 0; i < length && pc + i < 65536; i++) {
            bytes[pc + i] = 1;
        }
    }
    std::string text;
    std::uint32_t pc = 0;
    while (pc < 65536) {
        if (bytes[pc] == 0) {
            pc++;
            continue;
        }
        std::uint32_t first = pc;
        while (pc < 65536 && bytes[pc] != 0) {
            pc++;
        }
        char line[16];
        std::snprintf(line, sizeof(line), "%04X %04X\n", first, pc - 1);
        text += line;
    }
    return text;
}

} // namespace tisple
