#include "machine/via6522.hpp"

namespace tisple {

namespace {

bool FreeRun(const Via6522& v) {
    return (v.acr & 0x40) != 0;
}

// cycles between two underflows: the latch and 2 in free-run, else the full 16 bits
uint64_t T1Period(const Via6522& v) {
    return FreeRun(v) ? static_cast<uint64_t>(v.t1_latch) + 2 : 0x10000;
}

// the first T1 underflow at or after cycle
uint64_t T1UnderflowFrom(const Via6522& v, uint64_t cycle) {
    if (cycle <= v.t1_underflow) {
        return v.t1_underflow;
    }
    uint64_t period = T1Period(v);
    uint64_t periods = (cycle - v.t1_underflow + period - 1) / period;
    return v.t1_underflow + periods * period;
}

// before the latch or the mode changes: the next underflow comes when it would have, and
// the reloads after it use what is set from now
void RebaseT1(Via6522& v, uint64_t cycle) {
    v.t1_underflow = T1UnderflowFrom(v, cycle);
}

// where the T1 flag sets next, after cycle
void AimT1Flag(Via6522& v, uint64_t cycle) {
    bool counts = FreeRun(v) || v.t1_armed;
    v.t1_flag_at = counts ? T1UnderflowFrom(v, cycle + 1) : kViaNever;
}

// the flags that set up to and including this cycle
void Settle(Via6522& v, uint64_t cycle) {
    if (cycle >= v.t1_flag_at) {
        v.ifr |= kViaIrqT1;
        v.t1_armed = false;
        v.t1_flag_at = kViaNever;
        if (FreeRun(v)) {
            v.t1_flag_at = T1UnderflowFrom(v, cycle + 1);
        }
    }
    if (cycle >= v.t2_flag_at) {
        v.ifr |= kViaIrqT2;
        v.t2_flag_at = kViaNever;
    }
}

uint8_t Low(uint16_t value) {
    return static_cast<uint8_t>(value & 0xFF);
}

uint8_t High(uint16_t value) {
    return static_cast<uint8_t>(value >> 8);
}

uint8_t Pins(uint8_t out, uint8_t ddr, uint8_t in) {
    return static_cast<uint8_t>((out & ddr) | (in & ~ddr));
}

} // namespace

uint16_t ViaTimer1(const Via6522& v, uint64_t cycle) {
    if (cycle < v.t1_underflow || !FreeRun(v)) {
        return static_cast<uint16_t>(v.t1_underflow - cycle - 1);
    }
    uint64_t into = (cycle - v.t1_underflow) % T1Period(v);
    if (into == 0) {
        return 0xFFFF;
    }
    return static_cast<uint16_t>(v.t1_latch - (into - 1));
}

uint16_t ViaTimer2(const Via6522& v, uint64_t cycle) {
    return static_cast<uint16_t>(v.t2_underflow - cycle - 1);
}

uint8_t ViaPortA(const Via6522& v) {
    return Pins(v.ora, v.ddra, 0xFF);
}

uint8_t ViaPortB(const Via6522& v) {
    return Pins(v.orb, v.ddrb, 0xFF);
}

uint8_t ViaRead(Via6522& v, int reg, uint64_t cycle, uint8_t port_a_in, uint8_t port_b_in) {
    Settle(v, cycle);
    switch (reg & 0x0F) {
    case 0x0:
        return Pins(v.orb, v.ddrb, port_b_in);
    case 0x1:
    case 0xF:
        return Pins(v.ora, v.ddra, port_a_in);
    case 0x2:
        return v.ddrb;
    case 0x3:
        return v.ddra;
    case 0x4:
        v.ifr &= static_cast<uint8_t>(~kViaIrqT1);
        return Low(ViaTimer1(v, cycle));
    case 0x5:
        return High(ViaTimer1(v, cycle));
    case 0x6:
        return Low(v.t1_latch);
    case 0x7:
        return High(v.t1_latch);
    case 0x8:
        v.ifr &= static_cast<uint8_t>(~kViaIrqT2);
        return Low(ViaTimer2(v, cycle));
    case 0x9:
        return High(ViaTimer2(v, cycle));
    case 0xA:
        return v.sr;
    case 0xB:
        return v.acr;
    case 0xC:
        return v.pcr;
    case 0xD:
        return static_cast<uint8_t>(v.ifr | ((v.ifr & v.ier) != 0 ? 0x80 : 0x00));
    default:
        return static_cast<uint8_t>(v.ier | 0x80);
    }
}

void ViaWrite(Via6522& v, int reg, uint8_t value, uint64_t cycle) {
    Settle(v, cycle);
    switch (reg & 0x0F) {
    case 0x0:
        v.orb = value;
        break;
    case 0x1:
    case 0xF:
        v.ora = value;
        break;
    case 0x2:
        v.ddrb = value;
        break;
    case 0x3:
        v.ddra = value;
        break;
    case 0x4:
    case 0x6:
        RebaseT1(v, cycle);
        v.t1_latch = static_cast<uint16_t>((v.t1_latch & 0xFF00) | value);
        break;
    case 0x5:
        v.t1_latch = static_cast<uint16_t>((v.t1_latch & 0x00FF) | (value << 8));
        v.ifr &= static_cast<uint8_t>(~kViaIrqT1);
        v.t1_underflow = cycle + v.t1_latch + 2;
        v.t1_armed = true;
        v.t1_flag_at = v.t1_underflow;
        break;
    case 0x7:
        RebaseT1(v, cycle);
        v.t1_latch = static_cast<uint16_t>((v.t1_latch & 0x00FF) | (value << 8));
        v.ifr &= static_cast<uint8_t>(~kViaIrqT1);
        break;
    case 0x8:
        v.t2_latch_low = value;
        break;
    case 0x9:
        v.ifr &= static_cast<uint8_t>(~kViaIrqT2);
        v.t2_underflow = cycle + ((value << 8) | v.t2_latch_low) + 2;
        v.t2_flag_at = v.t2_underflow;
        break;
    case 0xA:
        v.sr = value;
        break;
    case 0xB:
        RebaseT1(v, cycle);
        v.acr = value;
        AimT1Flag(v, cycle);
        break;
    case 0xC:
        v.pcr = value;
        break;
    case 0xD:
        v.ifr &= static_cast<uint8_t>(~(value & 0x7F));
        break;
    default:
        if ((value & 0x80) != 0) {
            v.ier |= static_cast<uint8_t>(value & 0x7F);
        } else {
            v.ier &= static_cast<uint8_t>(~(value & 0x7F));
        }
        break;
    }
}

void ResetVia(Via6522& v, uint64_t cycle) {
    Settle(v, cycle);
    RebaseT1(v, cycle);
    v.orb = 0;
    v.ora = 0;
    v.ddrb = 0;
    v.ddra = 0;
    v.sr = 0;
    v.acr = 0;
    v.pcr = 0;
    v.ifr = 0;
    v.ier = 0;
    v.t1_armed = false;
    v.t1_flag_at = kViaNever;
    v.t2_flag_at = kViaNever;
}

uint64_t ViaIrqCycle(const Via6522& v) {
    if ((v.ifr & v.ier & 0x7F) != 0) {
        return 0;
    }
    uint64_t at = kViaNever;
    if ((v.ier & kViaIrqT1) != 0 && v.t1_flag_at < at) {
        at = v.t1_flag_at;
    }
    if ((v.ier & kViaIrqT2) != 0 && v.t2_flag_at < at) {
        at = v.t2_flag_at;
    }
    return at;
}

} // namespace tisple
