#include "emu/disasm.hpp"

#include "asm/opcodes.hpp"
#include "m6502dasm.h"

#include <array>
#include <cstdio>

namespace tisple {

namespace {

struct DasmState {
    const uint8_t* mem = nullptr;
    uint16_t next = 0;
    std::string text;
};

uint8_t DasmIn(void* user) {
    DasmState* state = static_cast<DasmState*>(user);
    return state->mem[state->next++];
}

void DasmOut(char c, void* user) {
    static_cast<DasmState*>(user)->text.push_back(c);
}

// the 151 documented NMOS opcodes (the core also implements the undocumented ones)
constexpr uint8_t kDocumented[] = {
    0x00, 0x01, 0x05, 0x06, 0x08, 0x09, 0x0a, 0x0d, 0x0e, 0x10, 0x11, 0x15, 0x16, 0x18, 0x19, 0x1d,
    0x1e, 0x20, 0x21, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2a, 0x2c, 0x2d, 0x2e, 0x30, 0x31, 0x35, 0x36,
    0x38, 0x39, 0x3d, 0x3e, 0x40, 0x41, 0x45, 0x46, 0x48, 0x49, 0x4a, 0x4c, 0x4d, 0x4e, 0x50, 0x51,
    0x55, 0x56, 0x58, 0x59, 0x5d, 0x5e, 0x60, 0x61, 0x65, 0x66, 0x68, 0x69, 0x6a, 0x6c, 0x6d, 0x6e,
    0x70, 0x71, 0x75, 0x76, 0x78, 0x79, 0x7d, 0x7e, 0x81, 0x84, 0x85, 0x86, 0x88, 0x8a, 0x8c, 0x8d,
    0x8e, 0x90, 0x91, 0x94, 0x95, 0x96, 0x98, 0x99, 0x9a, 0x9d, 0xa0, 0xa1, 0xa2, 0xa4, 0xa5, 0xa6,
    0xa8, 0xa9, 0xaa, 0xac, 0xad, 0xae, 0xb0, 0xb1, 0xb4, 0xb5, 0xb6, 0xb8, 0xb9, 0xba, 0xbc, 0xbd,
    0xbe, 0xc0, 0xc1, 0xc4, 0xc5, 0xc6, 0xc8, 0xc9, 0xca, 0xcc, 0xcd, 0xce, 0xd0, 0xd1, 0xd5, 0xd6,
    0xd8, 0xd9, 0xdd, 0xde, 0xe0, 0xe1, 0xe4, 0xe5, 0xe6, 0xe8, 0xe9, 0xea, 0xec, 0xed, 0xee, 0xf0,
    0xf1, 0xf5, 0xf6, 0xf8, 0xf9, 0xfd, 0xfe,
};

std::array<bool, 256> BuildDocumentedTable() {
    std::array<bool, 256> table = {};
    for (uint8_t opcode : kDocumented) {
        table[opcode] = true;
    }
    return table;
}

// the bytes of a 65C02 opcode that is not an instruction (cpu65c02.hpp: the documented NOPs)
int UndefinedLength65c02(uint8_t opcode) {
    int low = opcode & 0x0F;
    if (low == 0x02) {
        return 2;
    }
    if (opcode == 0x44 || opcode == 0x54 || opcode == 0xD4 || opcode == 0xF4) {
        return 2;
    }
    if (opcode == 0x5C || opcode == 0xDC || opcode == 0xFC) {
        return 3;
    }
    return 1;
}

std::string Operand65c02(Mode mode, uint16_t pc, uint8_t lo, uint8_t hi) {
    char text[32];
    unsigned word = static_cast<unsigned>(lo | (hi << 8));
    switch (mode) {
    case Mode::Imp:
    case Mode::Acc: // as the chips disassembler: ASL, INC
        return "";
    case Mode::Imm:
        std::snprintf(text, sizeof(text), " #$%02X", lo);
        break;
    case Mode::Zp:
        std::snprintf(text, sizeof(text), " $%02X", lo);
        break;
    case Mode::ZpX:
        std::snprintf(text, sizeof(text), " $%02X,X", lo);
        break;
    case Mode::ZpY:
        std::snprintf(text, sizeof(text), " $%02X,Y", lo);
        break;
    case Mode::Abs:
        std::snprintf(text, sizeof(text), " $%04X", word);
        break;
    case Mode::AbsX:
        std::snprintf(text, sizeof(text), " $%04X,X", word);
        break;
    case Mode::AbsY:
        std::snprintf(text, sizeof(text), " $%04X,Y", word);
        break;
    case Mode::Ind:
        std::snprintf(text, sizeof(text), " ($%04X)", word);
        break;
    case Mode::IndX:
        std::snprintf(text, sizeof(text), " ($%02X,X)", lo);
        break;
    case Mode::IndY:
        std::snprintf(text, sizeof(text), " ($%02X),Y", lo);
        break;
    case Mode::ZpInd:
        std::snprintf(text, sizeof(text), " ($%02X)", lo);
        break;
    case Mode::AbsIndX:
        std::snprintf(text, sizeof(text), " ($%04X,X)", word);
        break;
    default: // Rel
        std::snprintf(
            text, sizeof(text), " $%04X",
            static_cast<unsigned>(static_cast<uint16_t>(pc + 2 + static_cast<int8_t>(lo))));
        break;
    }
    return text;
}

std::string Disassemble65c02(const Machine& m, uint16_t pc, int& length) {
    uint8_t opcode = m.mem[pc];
    uint8_t lo = m.mem[static_cast<uint16_t>(pc + 1)];
    uint8_t hi = m.mem[static_cast<uint16_t>(pc + 2)];
    const OpcodeInfo& info = OpcodeInfoFor(opcode, Cpu::k65c02);
    if (!info.valid) {
        length = UndefinedLength65c02(opcode);
        return "*NOP";
    }
    length = ModeSize(info.mode);
    return std::string(MnName(info.mn)) + Operand65c02(info.mode, pc, lo, hi);
}

} // namespace

std::string Disassemble(const Machine& m, uint16_t pc, int& length) {
    if (m.cpu_kind == Cpu::k65c02) {
        return Disassemble65c02(m, pc, length);
    }
    DasmState state;
    state.mem = m.mem;
    state.next = pc;
    m6502dasm_op(pc, DasmIn, DasmOut, &state);
    length = static_cast<uint16_t>(state.next - pc);
    return state.text;
}

bool IsDocumentedOpcode(uint8_t opcode) {
    static const std::array<bool, 256> table = BuildDocumentedTable();
    return table[opcode];
}

bool IsDocumentedOpcode(uint8_t opcode, Cpu cpu) {
    if (cpu == Cpu::k65c02) {
        return OpcodeInfoFor(opcode, Cpu::k65c02).valid;
    }
    return IsDocumentedOpcode(opcode);
}

} // namespace tisple
