#include "emu/machine.hpp"

namespace tisple {

namespace {

void UpdateIrqDue(Machine& m) {
    uint64_t listed = m.irq_next < m.irq_count ? m.irq_cycles[m.irq_next] : UINT64_MAX;
    m.irq_due = listed < m.irq_level_at ? listed : m.irq_level_at;
}

bool InHook(const Machine& m, uint16_t addr) {
    return static_cast<uint32_t>(addr) - m.io.base < m.io.size;
}

// perform the access the cpu asked for on its pins
void DoBusCycle(Machine& m) {
    uint16_t addr = M6502_GET_ADDR(m.pins);
    bool is_read = (m.pins & M6502_RW) != 0;
    m.cycles++;
    bool hooked = m.io.size != 0 && InHook(m, addr);
    uint8_t value = 0;
    if (is_read) {
        value = hooked && m.io.read != nullptr ? m.io.read(m.io.ctx, addr) : m.mem[addr];
        M6502_SET_DATA(m.pins, value);
    } else {
        value = M6502_GET_DATA(m.pins);
        if (hooked && m.io.write != nullptr) {
            m.io.write(m.io.ctx, addr, value);
        } else {
            m.mem[addr] = value;
        }
    }
    if (m.bus_log != nullptr) {
        m.bus_log->push_back({addr, value, !is_read});
    }
}

} // namespace

void InitMachine(Machine& m) {
    m6502_desc_t desc = {};
    m.pins = m6502_init(&m.cpu, &desc);
    m.cycles = 0;
    SetRegs(m, Regs{});
}

void SetIoHook(Machine& m, const IoHook& hook) {
    m.io = hook;
}

void SetBusLog(Machine& m, std::vector<BusCycle>* log) {
    m.bus_log = log;
}

void SetRegs(Machine& m, const Regs& regs) {
    m6502_t& c = m.cpu;
    c.A = regs.a;
    c.X = regs.x;
    c.Y = regs.y;
    c.S = regs.s;
    // the core keeps B set and bit 5 clear inside P (so PHP/BRK push B); real chips have
    // neither bit, so outside we always show bit 5 set and B clear
    c.P = static_cast<uint8_t>((regs.p & ~kFlagU) | kFlagB);
    c.brk_flags = 0;
    c.irq_pip = 0;
    c.nmi_pip = 0;
    // present an opcode fetch at pc as the next bus cycle
    m.pins = M6502_SYNC | M6502_RW;
    M6502_SET_ADDR(m.pins, regs.pc);
    m6502_set_pc(&c, regs.pc);
    c.PINS = m.pins;
}

Regs GetRegs(const Machine& m) {
    Regs regs;
    regs.pc = M6502_GET_ADDR(m.pins);
    regs.a = m.cpu.A;
    regs.x = m.cpu.X;
    regs.y = m.cpu.Y;
    regs.s = m.cpu.S;
    regs.p = static_cast<uint8_t>((m.cpu.P & ~kFlagB) | kFlagU);
    return regs;
}

void StepInstruction(Machine& m) {
    // the fetch of the next instruction is the last tick, so stop when SYNC comes back
    bool taken = false;
    do {
        DoBusCycle(m);
        bool irq = m.cycles >= m.irq_due;
        if (irq) {
            m.pins |= M6502_IRQ;
        } else {
            m.pins &= ~M6502_IRQ;
        }
        m.pins = m6502_tick(&m.cpu, m.pins);
        // the cpu takes it (one a step: the sequence keeps the flag a few ticks): the line goes
        // high again
        if (irq && !taken && (m.cpu.brk_flags & M6502_BRK_IRQ) != 0) {
            taken = true;
            m.irqs_taken++;
            if (m.irq_next < m.irq_count && m.cycles >= m.irq_cycles[m.irq_next]) {
                m.irq_next++;
                UpdateIrqDue(m);
            }
        }
    } while ((m.pins & M6502_SYNC) == 0);
}

bool RequestIrq(Machine& m, uint64_t cycle) {
    if (m.irq_next == m.irq_count) {
        m.irq_next = 0; // all taken: start the list again
        m.irq_count = 0;
    }
    if (m.irq_count == Machine::kMaxIrqs) {
        return false;
    }
    m.irq_cycles[m.irq_count++] = cycle;
    UpdateIrqDue(m);
    return true;
}

void SetIrqLevel(Machine& m, uint64_t at) {
    m.irq_level_at = at;
    UpdateIrqDue(m);
}

} // namespace tisple
