#include "emu/cpu65c02.hpp"

#include "emu/cpu65c02_internal.hpp"

namespace tisple {

using namespace c02;

namespace {

// the effective address of the eight ALU modes by the opcode's low bits ($x1 (zp,X), $x5 zp,
// $x9 #, $xD abs, $x11 (zp),Y, $x15 zp,X, $x19 abs,Y, $x1D abs,X, $x12 (zp)); # gives pc
inline uint16_t AluAddr(Machine& m, uint16_t& pc, uint8_t opcode) {
    switch (opcode & 0x1F) {
    case 0x01:
        return AddrIndX(m, pc);
    case 0x05:
        return Rd(m, pc++);
    case 0x09:
        return pc++;
    case 0x0D:
        return AddrAbs(m, pc);
    case 0x11:
        return AddrIndY(m, pc, false);
    case 0x12:
        return AddrZpInd(m, pc);
    case 0x15:
        return AddrZpIndexed(m, pc, m.cpu.X);
    case 0x19:
        return AddrAbsIndexed(m, pc, m.cpu.Y, false);
    default: // 0x1D
        return AddrAbsIndexed(m, pc, m.cpu.X, false);
    }
}

// the same for STA, whose indexed modes always take the fix-up cycle
inline uint16_t StaAddr(Machine& m, uint16_t& pc, uint8_t opcode) {
    switch (opcode & 0x1F) {
    case 0x11:
        return AddrIndY(m, pc, true);
    case 0x19:
        return AddrAbsIndexed(m, pc, m.cpu.Y, true);
    case 0x1D:
        return AddrAbsIndexed(m, pc, m.cpu.X, true);
    default:
        return AluAddr(m, pc, opcode);
    }
}

// ORA AND EOR ADC (STA) LDA CMP SBC by the opcode's top three bits
void AluOp(Machine& m, uint16_t& pc, uint8_t opcode) {
    m6502_t& c = m.cpu;
    int group = opcode >> 5;
    if (group == 4) {
        Wr(m, StaAddr(m, pc, opcode), c.A);
        return;
    }
    uint16_t ea = AluAddr(m, pc, opcode);
    uint8_t v = Rd(m, ea);
    switch (group) {
    case 0:
        c.A = NZ(c, static_cast<uint8_t>(c.A | v));
        break;
    case 1:
        c.A = NZ(c, static_cast<uint8_t>(c.A & v));
        break;
    case 2:
        c.A = NZ(c, static_cast<uint8_t>(c.A ^ v));
        break;
    case 3:
    case 7: {
        bool decimal = (c.P & kD) != 0;
        if (group == 3) {
            Adc(c, v);
        } else {
            Sbc(c, v);
        }
        if (decimal) {
            Rd(m, ea); // the extra decimal cycle reads the operand's address again
        }
        break;
    }
    case 5:
        c.A = NZ(c, v);
        break;
    default: // 6
        Compare(c, c.A, v);
        break;
    }
}

// the interrupt sequence: the discarded fetch, a dummy read, PC and P (B clear), the vector
void Interrupt(Machine& m, uint16_t pc) {
    m6502_t& c = m.cpu;
    Fetch(m, pc);
    if (m.cycles >= m.irq_due) {
        NoteIrqTaken(m);
    }
    Rd(m, pc);
    Push(m, static_cast<uint8_t>(pc >> 8));
    Push(m, static_cast<uint8_t>(pc));
    Push(m, static_cast<uint8_t>((c.P | kU) & ~kB));
    c.P = static_cast<uint8_t>((c.P | kI) & ~kD);
    uint8_t lo = Rd(m, 0xFFFE);
    uint8_t hi = Rd(m, 0xFFFF);
    c.brk_flags = 0;
    c.PC = Word(lo, hi);
    m.pins = (m.pins & ~0xFFFFULL) | M6502_SYNC | M6502_RW | c.PC;
}

} // namespace

void Step65c02(Machine& m) {
    m6502_t& c = m.cpu;
    uint16_t pc = M6502_GET_ADDR(m.pins);
    if ((c.brk_flags & M6502_BRK_IRQ) != 0) {
        Interrupt(m, pc);
        return;
    }
    uint8_t opcode = Fetch(m, pc);
    pc++;
    int poll = 1;
    bool alu = ((opcode & 0x03) == 0x01 && opcode != 0x89) || (opcode & 0x1F) == 0x12;
    if (alu) {
        AluOp(m, pc, opcode);
    } else {
        poll = StepOther(m, pc, opcode);
    }
    c.PC = pc;
    m.pins = (m.pins & ~0xFFFFULL) | M6502_SYNC | M6502_RW | pc;
    if (poll >= 0 && ((c.irq_pip >> poll) & 1) != 0) {
        c.brk_flags |= M6502_BRK_IRQ;
    }
}

void Reset65c02(Machine& m) {
    m6502_t& c = m.cpu;
    uint16_t pc = M6502_GET_ADDR(m.pins);
    Fetch(m, pc);
    Rd(m, pc);
    for (int i = 0; i < 3; i++) {
        Rd(m, static_cast<uint16_t>(0x0100 | c.S));
        c.S--;
    }
    uint8_t lo = Rd(m, 0xFFFC);
    uint8_t hi = Rd(m, 0xFFFD);
    c.P = static_cast<uint8_t>((c.P | kI | kB) & ~(kD | kU));
    c.brk_flags = 0;
    c.irq_pip = 0;
    c.PC = Word(lo, hi);
    m.pins = (m.pins & ~0xFFFFULL) | M6502_SYNC | M6502_RW | c.PC;
}

} // namespace tisple
