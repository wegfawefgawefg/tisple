#pragma once

// The 65C02 core's bus cycles and addressing modes, shared by cpu65c02.cpp and
// cpu65c02_ops.cpp. Every access samples the interrupt line (machine.cpp polls the same way).

#include "emu/bus.hpp"
#include "emu/cpu65c02_alu.hpp"

namespace tisple::c02 {

// the line low and I clear at this cycle: one bit a cycle, the newest at the bottom
inline void SampleIrq(Machine& m) {
    bool low = m.cycles >= m.irq_due && (m.cpu.P & kI) == 0;
    m.cpu.irq_pip = static_cast<uint16_t>((m.cpu.irq_pip << 1) | (low ? 1 : 0));
}

inline uint8_t Rd(Machine& m, uint16_t addr) {
    m.pins = (m.pins & ~(M6502_SYNC | 0xFFFFULL)) | M6502_RW | addr;
    DoBusCycle(m);
    SampleIrq(m);
    return M6502_GET_DATA(m.pins);
}

inline void Wr(Machine& m, uint16_t addr, uint8_t v) {
    m.pins =
        (m.pins & ~(M6502_SYNC | M6502_RW | 0xFFFFFFULL)) | addr | (static_cast<uint64_t>(v) << 16);
    DoBusCycle(m);
    SampleIrq(m);
}

inline uint8_t Fetch(Machine& m, uint16_t pc) {
    m.pins = (m.pins & ~0xFFFFULL) | M6502_SYNC | M6502_RW | pc;
    DoBusCycle(m);
    SampleIrq(m);
    return M6502_GET_DATA(m.pins);
}

inline void Push(Machine& m, uint8_t v) {
    Wr(m, static_cast<uint16_t>(0x0100 | m.cpu.S), v);
    m.cpu.S--;
}

// ---- addressing: each returns the effective address after the mode's own cycles

inline uint16_t Word(uint8_t lo, uint8_t hi) {
    return static_cast<uint16_t>(lo | (hi << 8));
}

inline uint16_t AddrAbs(Machine& m, uint16_t& pc) {
    uint8_t lo = Rd(m, pc++);
    uint8_t hi = Rd(m, pc++);
    return Word(lo, hi);
}

inline uint16_t AddrZpIndexed(Machine& m, uint16_t& pc, uint8_t index) {
    uint8_t zp = Rd(m, pc++);
    Rd(m, zp);
    return static_cast<uint8_t>(zp + index);
}

// `always`: the fix-up cycle comes without a page crossing too (stores, INC and DEC); it
// reads the last operand byte again
inline uint16_t AddrAbsIndexed(Machine& m, uint16_t& pc, uint8_t index, bool always) {
    uint16_t base = AddrAbs(m, pc);
    uint16_t ea = static_cast<uint16_t>(base + index);
    if (always || ((base ^ ea) & 0xFF00) != 0) {
        Rd(m, static_cast<uint16_t>(pc - 1));
    }
    return ea;
}

inline uint16_t Pointer(Machine& m, uint8_t zp) {
    uint8_t lo = Rd(m, zp);
    uint8_t hi = Rd(m, static_cast<uint8_t>(zp + 1));
    return Word(lo, hi);
}

inline uint16_t AddrIndX(Machine& m, uint16_t& pc) {
    uint8_t zp = Rd(m, pc++);
    Rd(m, zp);
    return Pointer(m, static_cast<uint8_t>(zp + m.cpu.X));
}

inline uint16_t AddrIndY(Machine& m, uint16_t& pc, bool always) {
    uint8_t zp = Rd(m, pc++);
    uint16_t base = Pointer(m, zp);
    uint16_t ea = static_cast<uint16_t>(base + m.cpu.Y);
    if (always || ((base ^ ea) & 0xFF00) != 0) {
        Rd(m, static_cast<uint16_t>(pc - 1));
    }
    return ea;
}

inline uint16_t AddrZpInd(Machine& m, uint16_t& pc) {
    uint8_t zp = Rd(m, pc++);
    return Pointer(m, zp);
}

using RmwFn = uint8_t (*)(m6502_t&, uint8_t);

// read, read again, write
inline void Rmw(Machine& m, uint16_t ea, RmwFn fn) {
    uint8_t v = Rd(m, ea);
    Rd(m, ea);
    Wr(m, ea, fn(m.cpu, v));
}

// a taken branch: the next opcode's address read, and on a page crossing the half-formed
// target; returns false when it stays in its page (it polls at its first cycle then)
inline bool TakeBranch(Machine& m, uint16_t& pc, uint8_t offset) {
    Rd(m, pc);
    uint16_t target = static_cast<uint16_t>(pc + static_cast<int8_t>(offset));
    bool crossed = ((pc ^ target) & 0xFF00) != 0;
    if (crossed) {
        Rd(m, static_cast<uint16_t>((pc & 0xFF00) | (target & 0x00FF)));
    }
    pc = target;
    return crossed;
}

inline uint8_t Pull(Machine& m) {
    m.cpu.S++;
    return Rd(m, static_cast<uint16_t>(0x0100 | m.cpu.S));
}

// the opcodes outside the ALU columns (cpu65c02_ops.cpp); returns which sampled cycle polls
// interrupts (1: the second-to-last, 2: the third-to-last, -1: none)
int StepOther(Machine& m, uint16_t& pc, uint8_t opcode);

} // namespace tisple::c02
