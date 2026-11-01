#include "asm/cpu.hpp"

#include <cstring>

namespace tisple {

const char* CpuName(Cpu cpu) {
    return cpu == Cpu::k65c02 ? "65c02" : "6502";
}

bool ParseCpu(const char* text, Cpu& cpu) {
    if (std::strcmp(text, "6502") == 0 || std::strcmp(text, "nmos") == 0) {
        cpu = Cpu::k6502;
        return true;
    }
    if (std::strcmp(text, "65c02") == 0 || std::strcmp(text, "65C02") == 0 ||
        std::strcmp(text, "cmos") == 0) {
        cpu = Cpu::k65c02;
        return true;
    }
    return false;
}

} // namespace tisple
