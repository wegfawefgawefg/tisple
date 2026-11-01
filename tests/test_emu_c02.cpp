// the 65C02 core against the NMOS core where the two chips agree: interrupt timing on code
// whose instructions take the same cycles on both, and what an interrupt leaves behind

#include "emu/harness.hpp"
#include "test_emu.hpp"

#include <memory>
#include <vector>

namespace tisple_tests {

namespace {

// CLI, then a loop of loads, adds, stores, increments and taken and untaken branches; the
// handler at $0900 counts in $20 and returns. Cycles are the same on both chips (D clear).
const std::vector<uint8_t> kLoop = {
    0x58,             // $0800 CLI
    0xA9, 0x01,       // $0801 loop: LDA #1
    0x65, 0x10,       // $0803 ADC $10
    0x85, 0x10,       // $0805 STA $10
    0xA6, 0x10,       // $0807 LDX $10
    0xE8,             // $0809 INX
    0xD0, 0x01,       // $080A BNE +1
    0xEA,             // $080C NOP
    0xBD, 0xF8, 0x08, // $080D LDA $08F8,X (crosses a page for X >= 8)
    0x08,             // $0810 PHP
    0x28,             // $0811 PLP
    0x4C, 0x01, 0x08, // $0812 JMP loop
};
const std::vector<uint8_t> kHandler = {0x48, 0xE6, 0x20, 0x68, 0x40}; // PHA INC $20 PLA RTI

struct IrqTrace {
    uint64_t entry_cycle = 0; // the cycle count when the handler's first instruction starts
    uint16_t return_pc = 0;
    uint8_t pushed_p = 0;
    uint8_t p_in_handler = 0;
};

IrqTrace TraceIrq(tisple::Cpu cpu, uint64_t request, bool decimal) {
    auto m = std::make_unique<tisple::Machine>();
    tisple::InitMachine(*m, cpu);
    tisple::LoadBytes(*m, 0x0800, kLoop.data(), kLoop.size());
    tisple::LoadBytes(*m, 0x0900, kHandler.data(), kHandler.size());
    m->mem[0xFFFE] = 0x00;
    m->mem[0xFFFF] = 0x09;
    tisple::Regs regs;
    regs.pc = 0x0800;
    regs.s = 0xFF;
    regs.p = static_cast<uint8_t>(tisple::kFlagI | tisple::kFlagU | (decimal ? tisple::kFlagD : 0));
    tisple::SetRegs(*m, regs);
    tisple::RequestIrq(*m, request);
    IrqTrace t;
    while (m->cycles < request + 200) {
        if (M6502_GET_ADDR(m->pins) == 0x0900) {
            t.entry_cycle = m->cycles;
            uint8_t s = m->cpu.S;
            t.pushed_p = m->mem[0x0100 | static_cast<uint8_t>(s + 1)];
            t.return_pc =
                static_cast<uint16_t>(m->mem[0x0100 | static_cast<uint8_t>(s + 2)] |
                                      (m->mem[0x0100 | static_cast<uint8_t>(s + 3)] << 8));
            t.p_in_handler = tisple::GetRegs(*m).p;
            break;
        }
        tisple::StepInstruction(*m);
    }
    return t;
}

void TestIrqTimingMatches(TestResults& results) {
    int same = 0;
    int taken = 0;
    for (uint64_t request = 0; request < 120; request++) {
        IrqTrace nmos = TraceIrq(tisple::Cpu::k6502, request, false);
        IrqTrace cmos = TraceIrq(tisple::Cpu::k65c02, request, false);
        taken += nmos.entry_cycle != 0 ? 1 : 0;
        same += nmos.entry_cycle == cmos.entry_cycle && nmos.return_pc == cmos.return_pc &&
                        nmos.pushed_p == cmos.pushed_p
                    ? 1
                    : 0;
    }
    CHECK(results, taken == 120);
    CHECK(results, same == 120);
}

// both push P with D set; the 65C02 clears D for the handler, the NMOS chip keeps it
void TestIrqClearsDecimal(TestResults& results) {
    IrqTrace nmos = TraceIrq(tisple::Cpu::k6502, 40, true);
    IrqTrace cmos = TraceIrq(tisple::Cpu::k65c02, 40, true);
    CHECK(results, (nmos.pushed_p & tisple::kFlagD) != 0 && (cmos.pushed_p & tisple::kFlagD) != 0);
    CHECK(results, (nmos.p_in_handler & tisple::kFlagD) != 0);
    CHECK(results, (cmos.p_in_handler & tisple::kFlagD) == 0);
    CHECK(results, (cmos.pushed_p & tisple::kFlagB) == 0);
}

// a one-cycle NOP never lets an interrupt in right after it: the next instruction runs first
void TestOneCycleNop(TestResults& results) {
    auto m = std::make_unique<tisple::Machine>();
    tisple::InitMachine(*m, tisple::Cpu::k65c02);
    const std::vector<uint8_t> code = {0x58, 0xEA, 0x03, 0xE8, 0x4C, 0x04, 0x08};
    tisple::LoadBytes(*m, 0x0800, code.data(), code.size()); // CLI NOP NOP1 INX JMP *
    m->mem[0xFFFE] = 0x00;
    m->mem[0xFFFF] = 0x09;
    tisple::Regs regs;
    regs.pc = 0x0800;
    tisple::SetRegs(*m, regs);
    tisple::RequestIrq(*m, 4); // the NOP1 would poll at cycle 4 and take it
    std::vector<uint16_t> pcs;
    for (int i = 0; i < 5; i++) {
        pcs.push_back(M6502_GET_ADDR(m->pins));
        tisple::StepInstruction(*m);
    }
    // CLI, NOP, the 1-cycle NOP, INX, then the interrupt sequence
    CHECK(results, pcs.size() == 5 && pcs[2] == 0x0802 && pcs[3] == 0x0803 && pcs[4] == 0x0804);
    CHECK(results, M6502_GET_ADDR(m->pins) == 0x0900 && m->cpu.X == 1);
}

// the same reset vector on both; the 65C02 also clears D
void TestReset(TestResults& results) {
    for (tisple::Cpu cpu : {tisple::Cpu::k6502, tisple::Cpu::k65c02}) {
        auto m = std::make_unique<tisple::Machine>();
        tisple::InitMachine(*m, cpu);
        m->mem[0xFFFC] = 0x34;
        m->mem[0xFFFD] = 0x12;
        m->cpu.P |= tisple::kFlagD;
        m->cycles = 0;
        tisple::ResetCpu(*m);
        tisple::Regs regs = tisple::GetRegs(*m);
        CHECK(results, regs.pc == 0x1234 && m->cycles == 7 && (regs.p & tisple::kFlagI) != 0);
        if (cpu == tisple::Cpu::k65c02) {
            CHECK(results, (regs.p & tisple::kFlagD) == 0);
        }
    }
}

} // namespace

void RunEmu65c02Tests(TestResults& results) {
    TestIrqTimingMatches(results);
    TestIrqClearsDecimal(results);
    TestOneCycleNop(results);
    TestReset(results);
}

} // namespace tisple_tests
