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
    case Mn::STZ:
        Write(ins, cells, s, 0);
        break;
    case Mn::TSB:
    case Mn::TRB: {
        std::uint8_t m = Read(ins, cells, s);
        s.p = static_cast<std::uint8_t>((s.p & ~kZ) | ((s.a & m) == 0 ? kZ : 0));
        Write(ins, cells, s, static_cast<std::uint8_t>(ins.mn == Mn::TSB ? m | s.a : m & ~s.a));
        break;
    }
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
        if (ins.mode == Mode::Imm) {
            s.p = static_cast<std::uint8_t>((s.p & ~kZ) | ((s.a & m) == 0 ? kZ : 0));
            break;
        }
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

int InsCycles(const SoIns& ins, Cpu cpu) {
    int opcode = OpcodeFor(ins.mn, ins.mode, cpu);
    return opcode < 0 ? 0 : OpcodeInfoFor(static_cast<std::uint8_t>(opcode), cpu).cycles;
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
