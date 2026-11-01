#pragma once

// 64 KB flat memory around one of two cpu cores:
// - the NMOS 6502: the vendored chips m6502 core (third_party/chips), pin-level and
//   cycle-stepped; we perform every bus cycle it asks for, so dummy reads and the
//   read-modify-write double write happen like on the chip.
// - the 65C02 (cpu65c02.hpp): our own, an instruction at a time, doing the same bus cycles
//   in the same order through the same pins (its dummy reads are the 65C02's), so the rest
//   of the machine cannot tell the two apart but by what the chip does.

#include "asm/cpu.hpp"
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
    m6502_t cpu; // the registers of either core (the 65C02's keep the same conventions)
    Cpu cpu_kind = Cpu::k6502;
    // the bus request for the next cycle; between instructions it is the opcode fetch
    uint64_t pins = 0;
    uint64_t cycles = 0;
    uint8_t mem[65536] = {};
    IoHook io;
    // when set, every bus cycle is appended here
    std::vector<BusCycle>* bus_log = nullptr;
    // interrupt requests (tests): the IRQ line goes low at each of these cycle counts, in
    // ascending order, and stays low until the cpu takes the interrupt, as a device holds it
    // until it is answered; with I set it waits. A fixed array: the machine is copied whole
    // for snapshots
    static constexpr uint32_t kMaxIrqs = 1024;
    uint64_t irq_cycles[kMaxIrqs] = {};
    uint32_t irq_count = 0;
    uint32_t irq_next = 0;
    uint64_t irqs_taken = 0;
    // a device's IRQ output (the Mockingboard's 6522s), set with SetIrqLevel: the line is low
    // from this cycle count on, until the device moves it again; UINT64_MAX while it is high.
    // A level, not a request: the cpu takes it again after its RTI if the device still holds it
    uint64_t irq_level_at = UINT64_MAX;
    // the first cycle count either of the two holds the line low: one compare a cycle
    uint64_t irq_due = UINT64_MAX;
};

void InitMachine(Machine& m, Cpu cpu = Cpu::k6502);
// the chip's reset sequence: 7 cycles, then the vector at $FFFC (the 65C02's clears D too)
void ResetCpu(Machine& m);
void SetIoHook(Machine& m, const IoHook& hook);
void SetBusLog(Machine& m, std::vector<BusCycle>* log);

// registers are only meaningful between instructions
void SetRegs(Machine& m, const Regs& regs);
Regs GetRegs(const Machine& m);

// runs exactly one instruction, performing and counting all its bus cycles; a jammed cpu
// (an NMOS JAM opcode: it never fetches again) runs kMaxInstructionTicks cycles a call
constexpr int kMaxInstructionTicks = 32;
void StepInstruction(Machine& m);

// the NMOS opcodes that jam the chip ($02, $12, ... $F2 but $82, $A2, $C2, $E2); the 65C02
// has none
inline bool IsJamOpcode(uint8_t opcode) {
    return (opcode & 0x0F) == 0x02 && (opcode & 0x90) != 0x80;
}
inline bool IsJamOpcode(uint8_t opcode, Cpu cpu) {
    return cpu == Cpu::k6502 && IsJamOpcode(opcode);
}

// an interrupt request at cycle count `cycle` (after any already asked for); false when
// kMaxIrqs are waiting. Once every request has been taken the list starts again, so a test
// can go on asking one at a time
bool RequestIrq(Machine& m, uint64_t cycle);

// a device's IRQ output: low from cycle count `at` on (0: now; UINT64_MAX: high)
void SetIrqLevel(Machine& m, uint64_t at);

// the cpu begins an interrupt sequence: count it, and a listed request is answered (cores)
void NoteIrqTaken(Machine& m);

} // namespace tisple
