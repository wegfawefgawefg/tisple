#pragma once

// 64 KB flat memory around the vendored chips m6502 core (third_party/chips).
// the core is pin-level and cycle-stepped; we perform every bus cycle it asks for,
// so dummy reads and the read-modify-write double write happen like on the chip.

#include "m6502.h"

#include <cstdint>
#include <vector>

namespace tisple {

// status register bits
constexpr uint8_t kFlagC = 0x01;
constexpr uint8_t kFlagZ = 0x02;
constexpr uint8_t kFlagI = 0x04;
constexpr uint8_t kFlagD = 0x08;
constexpr uint8_t kFlagB = 0x10;
constexpr uint8_t kFlagU = 0x20;
constexpr uint8_t kFlagV = 0x40;
constexpr uint8_t kFlagN = 0x80;

struct Regs {
    uint16_t pc = 0;
    uint8_t a = 0;
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t s = 0xFD;
    uint8_t p = kFlagU | kFlagI;
};

// optional handler for an address range, e.g. the Apple II $C000-$CFFF I/O page.
// a null function means that direction falls through to plain RAM.
struct IoHook {
    uint32_t base = 0;
    uint32_t size = 0; // 0 means no hook
    uint8_t (*read)(void* ctx, uint16_t addr) = nullptr;
    void (*write)(void* ctx, uint16_t addr, uint8_t value) = nullptr;
    void* ctx = nullptr;
};

// one bus cycle as the chip drives it
struct BusCycle {
    uint16_t addr = 0;
    uint8_t value = 0;
    bool is_write = false;
};

struct Machine {
    m6502_t cpu;
    // the bus request for the next cycle; between instructions it is the opcode fetch
    uint64_t pins = 0;
    uint64_t cycles = 0;
    uint8_t mem[65536] = {};
    IoHook io;
    // when set, every bus cycle is appended here
    std::vector<BusCycle>* bus_log = nullptr;
};

void InitMachine(Machine& m);
void SetIoHook(Machine& m, const IoHook& hook);
void SetBusLog(Machine& m, std::vector<BusCycle>* log);

// registers are only meaningful between instructions
void SetRegs(Machine& m, const Regs& regs);
Regs GetRegs(const Machine& m);

// runs exactly one instruction, performing and counting all its bus cycles
void StepInstruction(Machine& m);

} // namespace tisple
