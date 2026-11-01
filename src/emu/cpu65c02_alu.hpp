#pragma once

// The 65C02's arithmetic and flag rules, for cpu65c02.cpp. P keeps the chips core's
// convention: B set and bit 5 clear inside the register.

#include "m6502.h"

#include <cstdint>

namespace tisple::c02 {

constexpr uint8_t kC = 0x01;
constexpr uint8_t kZ = 0x02;
constexpr uint8_t kI = 0x04;
constexpr uint8_t kD = 0x08;
constexpr uint8_t kB = 0x10;
constexpr uint8_t kU = 0x20;
constexpr uint8_t kV = 0x40;
constexpr uint8_t kN = 0x80;

inline void SetFlag(m6502_t& c, uint8_t flag, bool on) {
    c.P = static_cast<uint8_t>(on ? (c.P | flag) : (c.P & ~flag));
}

inline uint8_t NZ(m6502_t& c, uint8_t v) {
    c.P = static_cast<uint8_t>((c.P & ~(kN | kZ)) | (v == 0 ? kZ : (v & kN)));
    return v;
}

inline void AddBinary(m6502_t& c, uint8_t v) {
    unsigned sum = c.A + v + (c.P & kC);
    SetFlag(c, kV, ((~(c.A ^ v) & (c.A ^ sum)) & 0x80) != 0);
    SetFlag(c, kC, sum > 0xFF);
    c.A = NZ(c, static_cast<uint8_t>(sum));
}

// Bruce Clark, "Decimal Mode" (6502.org), appendix A: the accumulator and C by sequence 1, V by
// sequence 2, N and Z from the result
inline void AddDecimal(m6502_t& c, uint8_t v) {
    int carry = c.P & kC;
    int al = (c.A & 0x0F) + (v & 0x0F) + carry;
    if (al >= 0x0A) {
        al = ((al + 0x06) & 0x0F) + 0x10;
    }
    int sum = (c.A & 0xF0) + (v & 0xF0) + al;
    int signed_sum = static_cast<int8_t>(c.A & 0xF0) + static_cast<int8_t>(v & 0xF0) + al;
    SetFlag(c, kV, signed_sum < -128 || signed_sum > 127);
    if (sum >= 0xA0) {
        sum += 0x60;
    }
    SetFlag(c, kC, sum >= 0x100);
    c.A = NZ(c, static_cast<uint8_t>(sum));
}

// sequence 4 for the accumulator; C and V as in binary; N and Z from the result
inline void SubDecimal(m6502_t& c, uint8_t v) {
    int borrow = 1 - (c.P & kC);
    int al = (c.A & 0x0F) - (v & 0x0F) - borrow;
    int diff = c.A - v - borrow;
    int signed_diff = static_cast<int8_t>(c.A) - static_cast<int8_t>(v) - borrow;
    SetFlag(c, kV, signed_diff < -128 || signed_diff > 127);
    SetFlag(c, kC, diff >= 0);
    if (diff < 0) {
        diff -= 0x60;
    }
    if (al < 0) {
        diff -= 0x06;
    }
    c.A = NZ(c, static_cast<uint8_t>(diff));
}

inline void Adc(m6502_t& c, uint8_t v) {
    if ((c.P & kD) != 0) {
        AddDecimal(c, v);
    } else {
        AddBinary(c, v);
    }
}

inline void Sbc(m6502_t& c, uint8_t v) {
    if ((c.P & kD) != 0) {
        SubDecimal(c, v);
    } else {
        AddBinary(c, static_cast<uint8_t>(~v));
    }
}

inline void Compare(m6502_t& c, uint8_t reg, uint8_t v) {
    SetFlag(c, kC, reg >= v);
    NZ(c, static_cast<uint8_t>(reg - v));
}

inline void Bit(m6502_t& c, uint8_t v) {
    c.P =
        static_cast<uint8_t>((c.P & ~(kN | kV | kZ)) | (v & (kN | kV)) | ((c.A & v) == 0 ? kZ : 0));
}

// BIT #n: only Z
inline void BitImmediate(m6502_t& c, uint8_t v) {
    SetFlag(c, kZ, (c.A & v) == 0);
}

inline uint8_t Asl(m6502_t& c, uint8_t v) {
    SetFlag(c, kC, (v & 0x80) != 0);
    return NZ(c, static_cast<uint8_t>(v << 1));
}

inline uint8_t Lsr(m6502_t& c, uint8_t v) {
    SetFlag(c, kC, (v & 0x01) != 0);
    return NZ(c, static_cast<uint8_t>(v >> 1));
}

inline uint8_t Rol(m6502_t& c, uint8_t v) {
    uint8_t in = c.P & kC;
    SetFlag(c, kC, (v & 0x80) != 0);
    return NZ(c, static_cast<uint8_t>((v << 1) | in));
}

inline uint8_t Ror(m6502_t& c, uint8_t v) {
    uint8_t in = (c.P & kC) != 0 ? 0x80 : 0;
    SetFlag(c, kC, (v & 0x01) != 0);
    return NZ(c, static_cast<uint8_t>((v >> 1) | in));
}

inline uint8_t Inc(m6502_t& c, uint8_t v) {
    return NZ(c, static_cast<uint8_t>(v + 1));
}

inline uint8_t Dec(m6502_t& c, uint8_t v) {
    return NZ(c, static_cast<uint8_t>(v - 1));
}

// TSB and TRB: Z from A AND the byte, then the bits of A set or cleared in it
inline uint8_t Tsb(m6502_t& c, uint8_t v) {
    SetFlag(c, kZ, (c.A & v) == 0);
    return static_cast<uint8_t>(v | c.A);
}

inline uint8_t Trb(m6502_t& c, uint8_t v) {
    SetFlag(c, kZ, (c.A & v) == 0);
    return static_cast<uint8_t>(v & ~c.A);
}

} // namespace tisple::c02
