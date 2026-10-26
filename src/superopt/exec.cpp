#include "superopt/exec.hpp"

#include <cstdio>

namespace tisple {

namespace {

constexpr std::uint8_t kN = 0x80;
constexpr std::uint8_t kV = 0x40;
constexpr std::uint8_t kZ = 0x02;
constexpr std::uint8_t kC = 0x01;

void SetNZ(SoState& s, std::uint8_t v) {
    s.p = static_cast<std::uint8_t>((s.p & ~(kN | kZ)) | (v & kN) | (v == 0 ? kZ : 0));
}

void SetC(SoState& s, bool c) {
    s.p = static_cast<std::uint8_t>((s.p & ~kC) | (c ? kC : 0));
}

bool CellValid(const Cell& cell, const SoState& s) {
    switch (cell.index) {
    case CellIndex::X:
        return s.x == s.x0;
    case CellIndex::Y:
        return s.y == s.y0;
    default:
        return true;
    }
}

// the operand's byte; marks the state bad when a pseudo-cell's index moved
std::uint8_t Read(const SoIns& ins, const Cell* cells, SoState& s) {
    if (ins.mode == Mode::Imm) {
        return ins.imm;
    }
    if (ins.mode == Mode::Acc) {
        return s.a;
    }
    const Cell& cell = cells[ins.cell];
    if (!CellValid(cell, s)) {
        s.bad = 1;
    }
    return s.cells[ins.cell];
}

void Write(const SoIns& ins, const Cell* cells, SoState& s, std::uint8_t v) {
    if (ins.mode == Mode::Acc) {
        s.a = v;
        return;
    }
    if (!CellValid(cells[ins.cell], s)) {
        s.bad = 1;
    }
    s.cells[ins.cell] = v;
}

void Adc(SoState& s, std::uint8_t m) {
    unsigned sum = static_cast<unsigned>(s.a) + m + (s.p & kC);
    std::uint8_t r = static_cast<std::uint8_t>(sum);
    bool v = ((~(s.a ^ m)) & (s.a ^ r) & 0x80) != 0;
    s.p = static_cast<std::uint8_t>((s.p & ~kV) | (v ? kV : 0));
    SetC(s, sum > 0xFF);
    s.a = r;
    SetNZ(s, r);
}

void Compare(SoState& s, std::uint8_t reg, std::uint8_t m) {
    SetC(s, reg >= m);
    SetNZ(s, static_cast<std::uint8_t>(reg - m));
}

std::uint8_t Shift(SoState& s, Mn mn, std::uint8_t v) {
    std::uint8_t carry_in = s.p & kC;
    std::uint8_t r = 0;
    switch (mn) {
    case Mn::ASL:
        SetC(s, (v & 0x80) != 0);
        r = static_cast<std::uint8_t>(v << 1);
        break;
    case Mn::LSR:
        SetC(s, (v & 1) != 0);
        r = static_cast<std::uint8_t>(v >> 1);
        break;
    case Mn::ROL:
        SetC(s, (v & 0x80) != 0);
        r = static_cast<std::uint8_t>((v << 1) | carry_in);
        break;
    default: // ROR
        SetC(s, (v & 1) != 0);
        r = static_cast<std::uint8_t>((v >> 1) | (carry_in << 7));
        break;
    }
    SetNZ(s, r);
    return r;
}

std::string Hex(unsigned v, int digits) {
    char text[8];
    std::snprintf(text, sizeof(text), digits == 2 ? "$%02X" : "$%04X", v);
    return text;
}

} // namespace

bool IsDirectCell(const Cell& cell) {
    return cell.mode == Mode::Zp || cell.mode == Mode::Abs;
}

bool IsModelled(Mn mn) {
    switch (mn) {
    case Mn::ADC:
    case Mn::AND:
    case Mn::ASL:
    case Mn::BIT:
    case Mn::CLC:
    case Mn::CLV:
    case Mn::CMP:
    case Mn::CPX:
    case Mn::CPY:
    case Mn::DEC:
    case Mn::DEX:
    case Mn::DEY:
    case Mn::EOR:
    case Mn::INC:
    case Mn::INX:
    case Mn::INY:
    case Mn::LDA:
    case Mn::LDX:
    case Mn::LDY:
    case Mn::LSR:
    case Mn::ORA:
    case Mn::ROL:
    case Mn::ROR:
    case Mn::SBC:
    case Mn::SEC:
    case Mn::STA:
    case Mn::STX:
    case Mn::STY:
    case Mn::TAX:
    case Mn::TAY:
    case Mn::TXA:
    case Mn::TYA:
        return true;
    default:
        return false;
    }
}

LocEffects EffectsOf(Mn mn, Mode mode) {
    LocEffects e;
    bool memory = mode != Mode::Imp && mode != Mode::Acc && mode != Mode::Imm && mode != Mode::Rel;
    switch (mode) {
    case Mode::ZpX:
    case Mode::AbsX:
    case Mode::IndX:
        e.reads |= kLocX;
        break;
    case Mode::ZpY:
    case Mode::AbsY:
    case Mode::IndY:
        e.reads |= kLocY;
        break;
    default:
        break;
    }
    const std::uint8_t nz = kLocN | kLocZ;
    switch (mn) {
    case Mn::LDA:
        e.writes = kLocA | nz;
        break;
    case Mn::LDX:
        e.writes = kLocX | nz;
        break;
    case Mn::LDY:
        e.writes = kLocY | nz;
        break;
    case Mn::STA:
        e.reads |= kLocA;
        break;
    case Mn::STX:
        e.reads |= kLocX;
        break;
    case Mn::STY:
        e.reads |= kLocY;
        break;
    case Mn::ADC:
    case Mn::SBC:
        e.reads |= kLocA | kLocC;
        e.writes = kLocA | kLocFlags;
        break;
    case Mn::AND:
    case Mn::ORA:
    case Mn::EOR:
        e.reads |= kLocA;
        e.writes = kLocA | nz;
        break;
    case Mn::CMP:
        e.reads |= kLocA;
        e.writes = nz | kLocC;
        break;
    case Mn::CPX:
        e.reads |= kLocX;
        e.writes = nz | kLocC;
        break;
    case Mn::CPY:
        e.reads |= kLocY;
        e.writes = nz | kLocC;
        break;
    case Mn::BIT:
        e.reads |= kLocA;
        e.writes = nz | kLocV;
        break;
    case Mn::ASL:
    case Mn::LSR:
        e.reads |= mode == Mode::Acc ? kLocA : 0;
        e.writes = (mode == Mode::Acc ? kLocA : 0) | nz | kLocC;
        break;
    case Mn::ROL:
    case Mn::ROR:
        e.reads |= (mode == Mode::Acc ? kLocA : 0) | kLocC;
        e.writes = (mode == Mode::Acc ? kLocA : 0) | nz | kLocC;
        break;
    case Mn::INC:
    case Mn::DEC:
        e.writes = nz;
        break;
    case Mn::INX:
    case Mn::DEX:
        e.reads |= kLocX;
        e.writes = kLocX | nz;
        break;
    case Mn::INY:
    case Mn::DEY:
        e.reads |= kLocY;
        e.writes = kLocY | nz;
        break;
    case Mn::TAX:
        e.reads |= kLocA;
        e.writes = kLocX | nz;
        break;
    case Mn::TAY:
        e.reads |= kLocA;
        e.writes = kLocY | nz;
        break;
    case Mn::TXA:
        e.reads |= kLocX;
        e.writes = kLocA | nz;
        break;
    case Mn::TYA:
        e.reads |= kLocY;
        e.writes = kLocA | nz;
        break;
    case Mn::CLC:
    case Mn::SEC:
        e.writes = kLocC;
        break;
    case Mn::CLV:
        e.writes = kLocV;
        break;
    case Mn::BCC:
    case Mn::BCS:
        e.reads |= kLocC;
        break;
    case Mn::BEQ:
    case Mn::BNE:
        e.reads |= kLocZ;
        break;
    case Mn::BMI:
    case Mn::BPL:
        e.reads |= kLocN;
        break;
    case Mn::BVC:
    case Mn::BVS:
        e.reads |= kLocV;
        break;
    case Mn::PHA:
        e.reads |= kLocA;
        break;
    case Mn::PHP:
        e.reads |= kLocFlags;
        break;
    case Mn::PLA:
        e.writes = kLocA | nz;
        break;
    case Mn::PLP:
        e.writes = kLocFlags;
        break;
    case Mn::TSX:
        e.writes = kLocX | nz;
        break;
    case Mn::TXS:
        e.reads |= kLocX;
        break;
    default:
        break;
    }
    if (memory) {
        bool store = mn == Mn::STA || mn == Mn::STX || mn == Mn::STY;
        bool rmw = mn == Mn::INC || mn == Mn::DEC || mn == Mn::ASL || mn == Mn::LSR ||
                   mn == Mn::ROL || mn == Mn::ROR;
        bool jump = mn == Mn::JMP || mn == Mn::JSR;
        e.mem_read = !store && !jump;
        e.mem_write = store || rmw;
    }
    return e;
}

void Exec(const SoIns& ins, const Cell* cells, SoState& s) {
    switch (ins.mn) {
    case Mn::LDA:
        s.a = Read(ins, cells, s);
        SetNZ(s, s.a);
        break;
    case Mn::LDX:
        s.x = Read(ins, cells, s);
        SetNZ(s, s.x);
        break;
    case Mn::LDY:
        s.y = Read(ins, cells, s);
        SetNZ(s, s.y);
        break;
    case Mn::STA:
        Write(ins, cells, s, s.a);
        break;
    case Mn::STX:
        Write(ins, cells, s, s.x);
        break;
    case Mn::STY:
        Write(ins, cells, s, s.y);
        break;
    case Mn::ADC:
        Adc(s, Read(ins, cells, s));
        break;
    case Mn::SBC:
        Adc(s, static_cast<std::uint8_t>(~Read(ins, cells, s)));
        break;
    case Mn::AND:
        s.a &= Read(ins, cells, s);
        SetNZ(s, s.a);
        break;
    case Mn::ORA:
        s.a |= Read(ins, cells, s);
        SetNZ(s, s.a);
        break;
    case Mn::EOR:
        s.a ^= Read(ins, cells, s);
        SetNZ(s, s.a);
        break;
    case Mn::CMP:
        Compare(s, s.a, Read(ins, cells, s));
        break;
    case Mn::CPX:
        Compare(s, s.x, Read(ins, cells, s));
        break;
    case Mn::CPY:
        Compare(s, s.y, Read(ins, cells, s));
        break;
    case Mn::BIT: {
        std::uint8_t m = Read(ins, cells, s);
        s.p = static_cast<std::uint8_t>((s.p & ~(kN | kV | kZ)) | (m & (kN | kV)) |
                                        ((s.a & m) == 0 ? kZ : 0));
        break;
    }
    case Mn::ASL:
    case Mn::LSR:
    case Mn::ROL:
    case Mn::ROR:
        Write(ins, cells, s, Shift(s, ins.mn, Read(ins, cells, s)));
        break;
    case Mn::INC: {
        std::uint8_t v = static_cast<std::uint8_t>(Read(ins, cells, s) + 1);
        Write(ins, cells, s, v);
        SetNZ(s, v);
        break;
    }
    case Mn::DEC: {
        std::uint8_t v = static_cast<std::uint8_t>(Read(ins, cells, s) - 1);
        Write(ins, cells, s, v);
        SetNZ(s, v);
        break;
    }
    case Mn::INX:
        s.x++;
        SetNZ(s, s.x);
        break;
    case Mn::INY:
        s.y++;
        SetNZ(s, s.y);
        break;
    case Mn::DEX:
        s.x--;
        SetNZ(s, s.x);
        break;
    case Mn::DEY:
        s.y--;
        SetNZ(s, s.y);
        break;
    case Mn::TAX:
        s.x = s.a;
        SetNZ(s, s.x);
        break;
    case Mn::TAY:
        s.y = s.a;
        SetNZ(s, s.y);
        break;
    case Mn::TXA:
        s.a = s.x;
        SetNZ(s, s.a);
        break;
    case Mn::TYA:
        s.a = s.y;
        SetNZ(s, s.a);
        break;
    case Mn::CLC:
        SetC(s, false);
        break;
    case Mn::SEC:
        SetC(s, true);
        break;
    case Mn::CLV:
        s.p = static_cast<std::uint8_t>(s.p & ~kV);
        break;
    default:
        s.bad = 1;
        break;
    }
}

void ExecAll(const std::vector<SoIns>& code, const Cell* cells, SoState& s) {
    for (const SoIns& ins : code) {
        Exec(ins, cells, s);
    }
}

int InsCycles(const SoIns& ins) {
    int opcode = OpcodeFor(ins.mn, ins.mode);
    return opcode < 0 ? 0 : OpcodeInfoFor(static_cast<std::uint8_t>(opcode)).cycles;
}

int InsBytes(const SoIns& ins) {
    return ModeSize(ins.mode);
}

std::string InsText(const SoIns& ins, const Cell* cells) {
    std::string text = MnName(ins.mn);
    std::uint16_t v = ins.cell >= 0 ? cells[ins.cell].operand : 0;
    switch (ins.mode) {
    case Mode::Acc:
        return text + " A";
    case Mode::Imm:
        return text + " #" + Hex(ins.imm, 2);
    case Mode::Zp:
        return text + " " + Hex(v, 2);
    case Mode::ZpX:
        return text + " " + Hex(v, 2) + ",X";
    case Mode::ZpY:
        return text + " " + Hex(v, 2) + ",Y";
    case Mode::Abs:
        return text + " " + Hex(v, 4);
    case Mode::AbsX:
        return text + " " + Hex(v, 4) + ",X";
    case Mode::AbsY:
        return text + " " + Hex(v, 4) + ",Y";
    case Mode::IndX:
        return text + " (" + Hex(v, 2) + ",X)";
    case Mode::IndY:
        return text + " (" + Hex(v, 2) + "),Y";
    default:
        return text;
    }
}

std::string CodeText(const std::vector<SoIns>& code, const Cell* cells) {
    std::string text;
    for (std::size_t i = 0; i < code.size(); i++) {
        text += (i == 0 ? "" : " / ") + InsText(code[i], cells);
    }
    return code.empty() ? "(nothing)" : text;
}

} // namespace tisple
