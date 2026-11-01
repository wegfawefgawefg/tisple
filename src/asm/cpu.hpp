#pragma once

// Which chip: the NMOS 6502 every Apple II has, or the 65C02 of the enhanced IIe and the IIc
// (the 65C02 without Rockwell's bit instructions RMB/SMB/BBR/BBS and WDC's WAI/STP, which
// Apple's machines lack). The simulator runs either; the compiler targets either.

#include <cstdint>

namespace tisple {

enum class Cpu : std::uint8_t {
    k6502,
    k65c02,
};

// "6502" or "65c02"
const char* CpuName(Cpu cpu);
// "6502" (or "nmos"), "65c02" (or "cmos"); false for anything else
bool ParseCpu(const char* text, Cpu& cpu);

} // namespace tisple
