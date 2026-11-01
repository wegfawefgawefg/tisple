// the 65C02's opcodes outside the ALU columns: control flow, the stack, registers, the
// shift/increment column, the $x4 and $xC columns and the NOPs

#include "emu/cpu65c02_internal.hpp"

namespace tisple::c02 {

namespace {

// zp, zp,X, abs, abs,X of the shift and increment columns ($x6, $xE)
void RmwOp(Machine& m, uint16_t& pc, uint8_t opcode, RmwFn fn) {
    uint16_t ea = 0;
    switch (opcode & 0x1F) {
    case 0x06:
        ea = Rd(m, pc++);
        break;
    case 0x16:
        ea = AddrZpIndexed(m, pc, m.cpu.X);
        break;
    case 0x0E:
        ea = AddrAbs(m, pc);
        break;
    default: // 0x1E: INC and DEC always take the fix-up cycle, the shifts only on a crossing
        ea = AddrAbsIndexed(m, pc, m.cpu.X, opcode == 0xDE || opcode == 0xFE);
        break;
    }
    Rmw(m, ea, fn);
}

// an implied instruction's second cycle reads the next byte
inline void Implied(Machine& m, uint16_t pc) {
    Rd(m, pc);
}

int Branch(Machine& m, uint16_t& pc, bool taken) {
    uint8_t offset = Rd(m, pc++);
    if (!taken) {
        return 1;
    }
    return TakeBranch(m, pc, offset) ? 1 : 2;
}

void PushReg(Machine& m, uint16_t pc, uint8_t v) {
    Implied(m, pc);
    Push(m, v);
}

uint8_t PullReg(Machine& m, uint16_t pc) {
    Implied(m, pc);
    Rd(m, static_cast<uint16_t>(0x0100 | m.cpu.S));
    return Pull(m);
}

void Brk(Machine& m, uint16_t& pc) {
    m6502_t& c = m.cpu;
    Rd(m, pc++); // the signature byte
    Push(m, static_cast<uint8_t>(pc >> 8));
    Push(m, static_cast<uint8_t>(pc));
    Push(m, static_cast<uint8_t>(c.P | kU | kB));
    c.P = static_cast<uint8_t>((c.P | kI) & ~kD);
    uint8_t lo = Rd(m, 0xFFFE);
    uint8_t hi = Rd(m, 0xFFFF);
    pc = Word(lo, hi);
}

void Jsr(Machine& m, uint16_t& pc) {
    uint8_t lo = Rd(m, pc++);
    Rd(m, static_cast<uint16_t>(0x0100 | m.cpu.S));
    Push(m, static_cast<uint8_t>(pc >> 8));
    Push(m, static_cast<uint8_t>(pc));
    uint8_t hi = Rd(m, pc);
    pc = Word(lo, hi);
}

void Rts(Machine& m, uint16_t& pc) {
    Implied(m, pc);
    Rd(m, static_cast<uint16_t>(0x0100 | m.cpu.S));
    uint8_t lo = Pull(m);
    uint8_t hi = Pull(m);
    pc = Word(lo, hi);
    Rd(m, pc);
    pc++;
}

void Rti(Machine& m, uint16_t& pc) {
    m6502_t& c = m.cpu;
    Implied(m, pc);
    Rd(m, static_cast<uint16_t>(0x0100 | c.S));
    c.P = static_cast<uint8_t>((Pull(m) | kB) & ~kU);
    uint8_t lo = Pull(m);
    uint8_t hi = Pull(m);
    pc = Word(lo, hi);
}

// JMP (abs): the NMOS chip's wrong address (the pointer's page with the low byte wrapped) is
// read once, then the right one
void JmpIndirect(Machine& m, uint16_t& pc) {
    uint16_t ptr = AddrAbs(m, pc);
    uint8_t lo = Rd(m, ptr);
    Rd(m, static_cast<uint16_t>((ptr & 0xFF00) | ((ptr + 1) & 0x00FF)));
    uint8_t hi = Rd(m, static_cast<uint16_t>(ptr + 1));
    pc = Word(lo, hi);
}

// JMP (abs,X): the low operand byte is read again while X is added
void JmpIndexedIndirect(Machine& m, uint16_t& pc) {
    uint8_t lo = Rd(m, pc);
    uint8_t hi = Rd(m, static_cast<uint16_t>(pc + 1));
    Rd(m, pc);
    uint16_t ptr = static_cast<uint16_t>(Word(lo, hi) + m.cpu.X);
    uint8_t to_lo = Rd(m, ptr);
    uint8_t to_hi = Rd(m, static_cast<uint16_t>(ptr + 1));
    pc = Word(to_lo, to_hi);
}

uint8_t ReadAbsIndexed(Machine& m, uint16_t& pc, uint8_t index) {
    return Rd(m, AddrAbsIndexed(m, pc, index, false));
}

// the $x4 and $xC columns
int ColumnFour(Machine& m, uint16_t& pc, uint8_t opcode) {
    m6502_t& c = m.cpu;
    switch (opcode) {
    case 0x04:
        Rmw(m, Rd(m, pc++), Tsb);
        break;
    case 0x0C:
        Rmw(m, AddrAbs(m, pc), Tsb);
        break;
    case 0x14:
        Rmw(m, Rd(m, pc++), Trb);
        break;
    case 0x1C:
        Rmw(m, AddrAbs(m, pc), Trb);
        break;
    case 0x24:
        Bit(c, Rd(m, Rd(m, pc++)));
        break;
    case 0x2C:
        Bit(c, Rd(m, AddrAbs(m, pc)));
        break;
    case 0x34:
        Bit(c, Rd(m, AddrZpIndexed(m, pc, c.X)));
        break;
    case 0x3C:
        Bit(c, ReadAbsIndexed(m, pc, c.X));
        break;
    case 0x4C: {
        uint8_t lo = Rd(m, pc++);
        uint8_t hi = Rd(m, pc);
        pc = Word(lo, hi);
        break;
    }
    case 0x5C: { // NOP: 3 bytes, 8 cycles
        uint8_t lo = Rd(m, pc++);
        Rd(m, pc++);
        Rd(m, static_cast<uint16_t>(0xFF00 | lo));
        for (int i = 0; i < 4; i++) {
            Rd(m, 0xFFFF);
        }
        break;
    }
    case 0x6C:
        JmpIndirect(m, pc);
        break;
    case 0x7C:
        JmpIndexedIndirect(m, pc);
        break;
    case 0x64:
        Wr(m, Rd(m, pc++), 0);
        break;
    case 0x74:
        Wr(m, AddrZpIndexed(m, pc, c.X), 0);
        break;
    case 0x9C:
        Wr(m, AddrAbs(m, pc), 0);
        break;
    case 0x84:
        Wr(m, Rd(m, pc++), c.Y);
        break;
    case 0x94:
        Wr(m, AddrZpIndexed(m, pc, c.X), c.Y);
        break;
    case 0x8C:
        Wr(m, AddrAbs(m, pc), c.Y);
        break;
    case 0xA4:
        c.Y = NZ(c, Rd(m, Rd(m, pc++)));
        break;
    case 0xB4:
        c.Y = NZ(c, Rd(m, AddrZpIndexed(m, pc, c.X)));
        break;
    case 0xAC:
        c.Y = NZ(c, Rd(m, AddrAbs(m, pc)));
        break;
    case 0xBC:
        c.Y = NZ(c, ReadAbsIndexed(m, pc, c.X));
        break;
    case 0xC4:
        Compare(c, c.Y, Rd(m, Rd(m, pc++)));
        break;
    case 0xCC:
        Compare(c, c.Y, Rd(m, AddrAbs(m, pc)));
        break;
    case 0xE4:
        Compare(c, c.X, Rd(m, Rd(m, pc++)));
        break;
    case 0xEC:
        Compare(c, c.X, Rd(m, AddrAbs(m, pc)));
        break;
    case 0x44: // NOP zp
        Rd(m, Rd(m, pc++));
        break;
    case 0x54: // NOP zp,X
    case 0xD4:
    case 0xF4:
        Rd(m, AddrZpIndexed(m, pc, c.X));
        break;
    default: // $DC, $FC: NOP, 3 bytes, 4 cycles
        AddrAbs(m, pc);
        Rd(m, static_cast<uint16_t>(pc - 1));
        break;
    }
    return 1;
}

// the $x6 and $xE columns
void ColumnSix(Machine& m, uint16_t& pc, uint8_t opcode) {
    m6502_t& c = m.cpu;
    switch (opcode >> 5) {
    case 0:
        RmwOp(m, pc, opcode, Asl);
        return;
    case 1:
        RmwOp(m, pc, opcode, Rol);
        return;
    case 2:
        RmwOp(m, pc, opcode, Lsr);
        return;
    case 3:
        RmwOp(m, pc, opcode, Ror);
        return;
    case 6:
        RmwOp(m, pc, opcode, Dec);
        return;
    case 7:
        RmwOp(m, pc, opcode, Inc);
        return;
    default:
        break;
    }
    switch (opcode) {
    case 0x86:
        Wr(m, Rd(m, pc++), c.X);
        break;
    case 0x96:
        Wr(m, AddrZpIndexed(m, pc, c.Y), c.X);
        break;
    case 0x8E:
        Wr(m, AddrAbs(m, pc), c.X);
        break;
    case 0x9E:
        Wr(m, AddrAbsIndexed(m, pc, c.X, true), 0);
        break;
    case 0xA6:
        c.X = NZ(c, Rd(m, Rd(m, pc++)));
        break;
    case 0xB6:
        c.X = NZ(c, Rd(m, AddrZpIndexed(m, pc, c.Y)));
        break;
    case 0xAE:
        c.X = NZ(c, Rd(m, AddrAbs(m, pc)));
        break;
    default: // 0xBE
        c.X = NZ(c, ReadAbsIndexed(m, pc, c.Y));
        break;
    }
}

// the $xA column: accumulator shifts, INC A and DEC A, transfers, PHX/PHY/PLX/PLY
void ColumnTen(Machine& m, uint16_t& pc, uint8_t opcode) {
    m6502_t& c = m.cpu;
    switch (opcode) {
    case 0x5A:
        PushReg(m, pc, c.Y);
        return;
    case 0x7A:
        c.Y = NZ(c, PullReg(m, pc));
        return;
    case 0xDA:
        PushReg(m, pc, c.X);
        return;
    case 0xFA:
        c.X = NZ(c, PullReg(m, pc));
        return;
    default:
        break;
    }
    Implied(m, pc);
    switch (opcode) {
    case 0x0A:
        c.A = Asl(c, c.A);
        break;
    case 0x1A:
        c.A = Inc(c, c.A);
        break;
    case 0x2A:
        c.A = Rol(c, c.A);
        break;
    case 0x3A:
        c.A = Dec(c, c.A);
        break;
    case 0x4A:
        c.A = Lsr(c, c.A);
        break;
    case 0x6A:
        c.A = Ror(c, c.A);
        break;
    case 0x8A:
        c.A = NZ(c, c.X);
        break;
    case 0x9A:
        c.S = c.X;
        break;
    case 0xAA:
        c.X = NZ(c, c.A);
        break;
    case 0xBA:
        c.X = NZ(c, c.S);
        break;
    case 0xCA:
        c.X = NZ(c, static_cast<uint8_t>(c.X - 1));
        break;
    default: // NOP $EA
        break;
    }
}

// the $x0 and $x8 columns
int ColumnZero(Machine& m, uint16_t& pc, uint8_t opcode) {
    m6502_t& c = m.cpu;
    switch (opcode) {
    case 0x00:
        Brk(m, pc);
        return -1; // no poll at the end of an interrupt sequence
    case 0x20:
        Jsr(m, pc);
        return 1;
    case 0x40:
        Rti(m, pc);
        return 1;
    case 0x60:
        Rts(m, pc);
        return 1;
    case 0x08:
        PushReg(m, pc, static_cast<uint8_t>(c.P | kU | kB));
        return 1;
    case 0x28:
        c.P = static_cast<uint8_t>((PullReg(m, pc) | kB) & ~kU);
        return 1;
    case 0x48:
        PushReg(m, pc, c.A);
        return 1;
    case 0x68:
        c.A = NZ(c, PullReg(m, pc));
        return 1;
    case 0x10:
        return Branch(m, pc, (c.P & kN) == 0);
    case 0x30:
        return Branch(m, pc, (c.P & kN) != 0);
    case 0x50:
        return Branch(m, pc, (c.P & kV) == 0);
    case 0x70:
        return Branch(m, pc, (c.P & kV) != 0);
    case 0x80:
        return Branch(m, pc, true);
    case 0x90:
        return Branch(m, pc, (c.P & kC) == 0);
    case 0xB0:
        return Branch(m, pc, (c.P & kC) != 0);
    case 0xD0:
        return Branch(m, pc, (c.P & kZ) == 0);
    case 0xF0:
        return Branch(m, pc, (c.P & kZ) != 0);
    case 0xA0:
        c.Y = NZ(c, Rd(m, pc++));
        return 1;
    case 0xC0:
        Compare(c, c.Y, Rd(m, pc++));
        return 1;
    case 0xE0:
        Compare(c, c.X, Rd(m, pc++));
        return 1;
    default:
        break;
    }
    Implied(m, pc);
    switch (opcode) {
    case 0x18:
        c.P &= static_cast<uint8_t>(~kC);
        break;
    case 0x38:
        c.P |= kC;
        break;
    case 0x58:
        c.P &= static_cast<uint8_t>(~kI);
        break;
    case 0x78:
        c.P |= kI;
        break;
    case 0xB8:
        c.P &= static_cast<uint8_t>(~kV);
        break;
    case 0xD8:
        c.P &= static_cast<uint8_t>(~kD);
        break;
    case 0xF8:
        c.P |= kD;
        break;
    case 0x88:
        c.Y = NZ(c, static_cast<uint8_t>(c.Y - 1));
        break;
    case 0x98:
        c.A = NZ(c, c.Y);
        break;
    case 0xA8:
        c.Y = NZ(c, c.A);
        break;
    case 0xC8:
        c.Y = NZ(c, static_cast<uint8_t>(c.Y + 1));
        break;
    default: // 0xE8
        c.X = NZ(c, static_cast<uint8_t>(c.X + 1));
        break;
    }
    return 1;
}

} // namespace

int StepOther(Machine& m, uint16_t& pc, uint8_t opcode) {
    m6502_t& c = m.cpu;
    switch (opcode & 0x0F) {
    case 0x00:
    case 0x08:
        return ColumnZero(m, pc, opcode);
    case 0x02:
        if (opcode == 0xA2) {
            c.X = NZ(c, Rd(m, pc++));
        } else {
            Rd(m, pc++); // NOP #: 2 bytes, 2 cycles
        }
        return 1;
    case 0x03:
    case 0x07:
    case 0x0B:
    case 0x0F:
        return -1; // 1-byte, 1-cycle NOPs: no interrupt starts right after one
    case 0x04:
    case 0x0C:
        return ColumnFour(m, pc, opcode);
    case 0x06:
    case 0x0E:
        ColumnSix(m, pc, opcode);
        return 1;
    case 0x09: // only $89 comes here: BIT #
        BitImmediate(c, Rd(m, pc++));
        return 1;
    default: // 0x0A
        ColumnTen(m, pc, opcode);
        return 1;
    }
}

} // namespace tisple::c02
