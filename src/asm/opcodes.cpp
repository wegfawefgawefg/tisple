#include "asm/opcodes.hpp"

#include <array>

namespace tisple {

namespace {

struct Entry {
    Mn mn;
    Mode mode;
    std::uint8_t opcode;
    std::uint8_t cycles;
    bool page_penalty;
};

// the eight standard modes of the ALU group: imm zp zp,x abs abs,x abs,y (zp,x) (zp),y
#define TISPLE_ALU(name, imm, zp, zpx, abs, absx, absy, indx, indy)                                \
    {Mn::name, Mode::Imm, imm, 2, false}, {Mn::name, Mode::Zp, zp, 3, false},                      \
        {Mn::name, Mode::ZpX, zpx, 4, false}, {Mn::name, Mode::Abs, abs, 4, false},                \
        {Mn::name, Mode::AbsX, absx, 4, true}, {Mn::name, Mode::AbsY, absy, 4, true},              \
        {Mn::name, Mode::IndX, indx, 6, false}, {                                                  \
        Mn::name, Mode::IndY, indy, 5, true                                                        \
    }

// read-modify-write shifts and increments: zp zp,x abs abs,x
#define TISPLE_RMW(name, zp, zpx, abs, absx)                                                       \
    {Mn::name, Mode::Zp, zp, 5, false}, {Mn::name, Mode::ZpX, zpx, 6, false},                      \
        {Mn::name, Mode::Abs, abs, 6, false}, {                                                    \
        Mn::name, Mode::AbsX, absx, 7, false                                                       \
    }

const Entry kEntries[] = {
    TISPLE_ALU(ADC, 0x69, 0x65, 0x75, 0x6D, 0x7D, 0x79, 0x61, 0x71),
    TISPLE_ALU(AND, 0x29, 0x25, 0x35, 0x2D, 0x3D, 0x39, 0x21, 0x31),
    TISPLE_ALU(CMP, 0xC9, 0xC5, 0xD5, 0xCD, 0xDD, 0xD9, 0xC1, 0xD1),
    TISPLE_ALU(EOR, 0x49, 0x45, 0x55, 0x4D, 0x5D, 0x59, 0x41, 0x51),
    TISPLE_ALU(LDA, 0xA9, 0xA5, 0xB5, 0xAD, 0xBD, 0xB9, 0xA1, 0xB1),
    TISPLE_ALU(ORA, 0x09, 0x05, 0x15, 0x0D, 0x1D, 0x19, 0x01, 0x11),
    TISPLE_ALU(SBC, 0xE9, 0xE5, 0xF5, 0xED, 0xFD, 0xF9, 0xE1, 0xF1),
    {Mn::ASL, Mode::Acc, 0x0A, 2, false},
    TISPLE_RMW(ASL, 0x06, 0x16, 0x0E, 0x1E),
    {Mn::LSR, Mode::Acc, 0x4A, 2, false},
    TISPLE_RMW(LSR, 0x46, 0x56, 0x4E, 0x5E),
    {Mn::ROL, Mode::Acc, 0x2A, 2, false},
    TISPLE_RMW(ROL, 0x26, 0x36, 0x2E, 0x3E),
    {Mn::ROR, Mode::Acc, 0x6A, 2, false},
    TISPLE_RMW(ROR, 0x66, 0x76, 0x6E, 0x7E),
    TISPLE_RMW(DEC, 0xC6, 0xD6, 0xCE, 0xDE),
    TISPLE_RMW(INC, 0xE6, 0xF6, 0xEE, 0xFE),
    {Mn::STA, Mode::Zp, 0x85, 3, false},
    {Mn::STA, Mode::ZpX, 0x95, 4, false},
    {Mn::STA, Mode::Abs, 0x8D, 4, false},
    {Mn::STA, Mode::AbsX, 0x9D, 5, false},
    {Mn::STA, Mode::AbsY, 0x99, 5, false},
    {Mn::STA, Mode::IndX, 0x81, 6, false},
    {Mn::STA, Mode::IndY, 0x91, 6, false},
    {Mn::BCC, Mode::Rel, 0x90, 2, false},
    {Mn::BCS, Mode::Rel, 0xB0, 2, false},
    {Mn::BEQ, Mode::Rel, 0xF0, 2, false},
    {Mn::BMI, Mode::Rel, 0x30, 2, false},
    {Mn::BNE, Mode::Rel, 0xD0, 2, false},
    {Mn::BPL, Mode::Rel, 0x10, 2, false},
    {Mn::BVC, Mode::Rel, 0x50, 2, false},
    {Mn::BVS, Mode::Rel, 0x70, 2, false},
    {Mn::BIT, Mode::Zp, 0x24, 3, false},
    {Mn::BIT, Mode::Abs, 0x2C, 4, false},
    {Mn::BRK, Mode::Imp, 0x00, 7, false},
    {Mn::CLC, Mode::Imp, 0x18, 2, false},
    {Mn::CLD, Mode::Imp, 0xD8, 2, false},
    {Mn::CLI, Mode::Imp, 0x58, 2, false},
    {Mn::CLV, Mode::Imp, 0xB8, 2, false},
    {Mn::CPX, Mode::Imm, 0xE0, 2, false},
    {Mn::CPX, Mode::Zp, 0xE4, 3, false},
    {Mn::CPX, Mode::Abs, 0xEC, 4, false},
    {Mn::CPY, Mode::Imm, 0xC0, 2, false},
    {Mn::CPY, Mode::Zp, 0xC4, 3, false},
    {Mn::CPY, Mode::Abs, 0xCC, 4, false},
    {Mn::DEX, Mode::Imp, 0xCA, 2, false},
    {Mn::DEY, Mode::Imp, 0x88, 2, false},
    {Mn::INX, Mode::Imp, 0xE8, 2, false},
    {Mn::INY, Mode::Imp, 0xC8, 2, false},
    {Mn::JMP, Mode::Abs, 0x4C, 3, false},
    {Mn::JMP, Mode::Ind, 0x6C, 5, false},
    {Mn::JSR, Mode::Abs, 0x20, 6, false},
    {Mn::LDX, Mode::Imm, 0xA2, 2, false},
    {Mn::LDX, Mode::Zp, 0xA6, 3, false},
    {Mn::LDX, Mode::ZpY, 0xB6, 4, false},
    {Mn::LDX, Mode::Abs, 0xAE, 4, false},
    {Mn::LDX, Mode::AbsY, 0xBE, 4, true},
    {Mn::LDY, Mode::Imm, 0xA0, 2, false},
    {Mn::LDY, Mode::Zp, 0xA4, 3, false},
    {Mn::LDY, Mode::ZpX, 0xB4, 4, false},
    {Mn::LDY, Mode::Abs, 0xAC, 4, false},
    {Mn::LDY, Mode::AbsX, 0xBC, 4, true},
    {Mn::NOP, Mode::Imp, 0xEA, 2, false},
    {Mn::PHA, Mode::Imp, 0x48, 3, false},
    {Mn::PHP, Mode::Imp, 0x08, 3, false},
    {Mn::PLA, Mode::Imp, 0x68, 4, false},
    {Mn::PLP, Mode::Imp, 0x28, 4, false},
    {Mn::RTI, Mode::Imp, 0x40, 6, false},
    {Mn::RTS, Mode::Imp, 0x60, 6, false},
    {Mn::SEC, Mode::Imp, 0x38, 2, false},
    {Mn::SED, Mode::Imp, 0xF8, 2, false},
    {Mn::SEI, Mode::Imp, 0x78, 2, false},
    {Mn::STX, Mode::Zp, 0x86, 3, false},
    {Mn::STX, Mode::ZpY, 0x96, 4, false},
    {Mn::STX, Mode::Abs, 0x8E, 4, false},
    {Mn::STY, Mode::Zp, 0x84, 3, false},
    {Mn::STY, Mode::ZpX, 0x94, 4, false},
    {Mn::STY, Mode::Abs, 0x8C, 4, false},
    {Mn::TAX, Mode::Imp, 0xAA, 2, false},
    {Mn::TAY, Mode::Imp, 0xA8, 2, false},
    {Mn::TSX, Mode::Imp, 0xBA, 2, false},
    {Mn::TXA, Mode::Imp, 0x8A, 2, false},
    {Mn::TXS, Mode::Imp, 0x9A, 2, false},
    {Mn::TYA, Mode::Imp, 0x98, 2, false},
};

#undef TISPLE_ALU
#undef TISPLE_RMW

constexpr int kMnCount = static_cast<int>(Mn::Count);
constexpr int kModeCount = static_cast<int>(Mode::Count);

struct Tables {
    std::array<std::array<int, kModeCount>, kMnCount> by_mode;
    std::array<OpcodeInfo, 256> info;
};

Tables BuildTables() {
    Tables t;
    for (auto& row : t.by_mode) {
        row.fill(-1);
    }
    for (const Entry& e : kEntries) {
        t.by_mode[static_cast<int>(e.mn)][static_cast<int>(e.mode)] = e.opcode;
        OpcodeInfo& info = t.info[e.opcode];
        info.mn = e.mn;
        info.mode = e.mode;
        info.cycles = e.cycles;
        info.page_penalty = e.page_penalty;
        info.valid = true;
    }
    return t;
}

const Tables& GetTables() {
    static const Tables tables = BuildTables();
    return tables;
}

const char* const kNames[] = {
    "ADC", "AND", "ASL", "BCC", "BCS", "BEQ", "BIT", "BMI", "BNE", "BPL", "BRK", "BVC",
    "BVS", "CLC", "CLD", "CLI", "CLV", "CMP", "CPX", "CPY", "DEC", "DEX", "DEY", "EOR",
    "INC", "INX", "INY", "JMP", "JSR", "LDA", "LDX", "LDY", "LSR", "NOP", "ORA", "PHA",
    "PHP", "PLA", "PLP", "ROL", "ROR", "RTI", "RTS", "SBC", "SEC", "SED", "SEI", "STA",
    "STX", "STY", "TAX", "TAY", "TSX", "TXA", "TXS", "TYA",
};

static_assert(sizeof(kNames) / sizeof(kNames[0]) == kMnCount);

} // namespace

int OpcodeFor(Mn mn, Mode mode) {
    return GetTables().by_mode[static_cast<int>(mn)][static_cast<int>(mode)];
}

const OpcodeInfo& OpcodeInfoFor(std::uint8_t opcode) {
    return GetTables().info[opcode];
}

const char* MnName(Mn mn) {
    return kNames[static_cast<int>(mn)];
}

int ModeSize(Mode mode) {
    switch (mode) {
    case Mode::Imp:
    case Mode::Acc:
        return 1;
    case Mode::Abs:
    case Mode::AbsX:
    case Mode::AbsY:
    case Mode::Ind:
        return 3;
    default:
        return 2;
    }
}

bool IsBranch(Mn mn) {
    switch (mn) {
    case Mn::BCC:
    case Mn::BCS:
    case Mn::BEQ:
    case Mn::BMI:
    case Mn::BNE:
    case Mn::BPL:
    case Mn::BVC:
    case Mn::BVS:
        return true;
    default:
        return false;
    }
}

Mn InvertBranch(Mn mn) {
    switch (mn) {
    case Mn::BCC:
        return Mn::BCS;
    case Mn::BCS:
        return Mn::BCC;
    case Mn::BEQ:
        return Mn::BNE;
    case Mn::BNE:
        return Mn::BEQ;
    case Mn::BMI:
        return Mn::BPL;
    case Mn::BPL:
        return Mn::BMI;
    case Mn::BVC:
        return Mn::BVS;
    case Mn::BVS:
        return Mn::BVC;
    default:
        return mn;
    }
}

} // namespace tisple
