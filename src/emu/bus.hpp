#pragma once

// One bus cycle as the cpu drives it on its pins, for both cores (machine.cpp, cpu65c02.cpp):
// the I/O hook or plain memory, the cycle count and the bus log.

#include "emu/machine.hpp"

namespace tisple {

inline bool InIoHook(const Machine& m, uint16_t addr) {
    return static_cast<uint32_t>(addr) - m.io.base < m.io.size;
}

// perform the access the cpu asked for on its pins
inline void DoBusCycle(Machine& m) {
    uint16_t addr = M6502_GET_ADDR(m.pins);
    bool is_read = (m.pins & M6502_RW) != 0;
    m.cycles++;
    bool hooked = m.io.size != 0 && InIoHook(m, addr);
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

} // namespace tisple
