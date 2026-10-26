// small hand-written simulator tests that always run

#include "emu/disasm.hpp"
#include "emu/harness.hpp"
#include "test_emu.hpp"

#include <memory>
#include <vector>

namespace tisple_tests {

namespace {

struct Sim {
    std::unique_ptr<tisple::Machine> machine = std::make_unique<tisple::Machine>();

    Sim() {
        tisple::InitMachine(*machine);
    }

    tisple::RunResult RunBytes(uint16_t addr, std::vector<uint8_t> bytes) {
        tisple::LoadBytes(*machine, addr, bytes.data(), bytes.size());
        tisple::Regs regs;
        regs.pc = addr;
        tisple::SetRegs(*machine, regs);
        return tisple::Run(*machine, tisple::RunOptions{});
    }
};

void TestCountdownLoop(TestResults& results) {
    Sim sim;
    // LDX #10 ; loop: DEX ; BNE loop ; BRK
    tisple::RunResult r = sim.RunBytes(0x0800, {0xA2, 0x0A, 0xCA, 0xD0, 0xFD, 0x00});
    CHECK(results, r.reason == tisple::StopReason::kBrk);
    CHECK(results, r.regs.x == 0);
    CHECK(results, r.regs.pc == 0x0805);
    CHECK(results, r.cycles == 2 + 10 * 2 + 9 * 3 + 2);
}

void TestDecimalAdd(TestResults& results) {
    Sim sim;
    // SED ; CLC ; LDA #$19 ; ADC #$28 ; BRK
    tisple::RunResult r = sim.RunBytes(0x0800, {0xF8, 0x18, 0xA9, 0x19, 0x69, 0x28, 0x00});
    CHECK(results, r.regs.a == 0x47);
    CHECK(results, (r.regs.p & tisple::kFlagC) == 0);
    CHECK(results, r.cycles == 8);

    // $99 + $01 wraps to $00 with carry
    tisple::RunResult wrap = sim.RunBytes(0x0900, {0xF8, 0x18, 0xA9, 0x99, 0x69, 0x01, 0x00});
    CHECK(results, wrap.regs.a == 0x00);
    CHECK(results, (wrap.regs.p & tisple::kFlagC) != 0);
}

void TestBranchCycles(TestResults& results) {
    Sim sim;
    // LDA #1 ; BNE +$20 at $08F0: taken, crosses to page $09 (4 cycles)
    sim.machine->mem[0x0912] = 0x00;
    tisple::RunResult crossing = sim.RunBytes(0x08EE, {0xA9, 0x01, 0xD0, 0x20});
    CHECK(results, crossing.regs.pc == 0x0912);
    CHECK(results, crossing.cycles == 2 + 4);

    // taken in the same page: 3 cycles
    tisple::RunResult same = sim.RunBytes(0x0800, {0xA9, 0x01, 0xD0, 0x05});
    CHECK(results, same.regs.pc == 0x0809);
    CHECK(results, same.cycles == 2 + 3);

    // not taken: 2 cycles
    tisple::RunResult skipped = sim.RunBytes(0x0A00, {0xA9, 0x00, 0xD0, 0x05, 0x00});
    CHECK(results, skipped.regs.pc == 0x0A04);
    CHECK(results, skipped.cycles == 2 + 2);
}

void TestStopAndLimit(TestResults& results) {
    Sim sim;
    // JMP $0800 forever
    std::vector<uint8_t> bytes = {0x4C, 0x00, 0x08};
    tisple::LoadBytes(*sim.machine, 0x0800, bytes.data(), bytes.size());
    tisple::Regs regs;
    regs.pc = 0x0800;
    tisple::SetRegs(*sim.machine, regs);
    tisple::RunOptions options;
    options.max_cycles = 30;
    tisple::RunResult r = tisple::Run(*sim.machine, options);
    CHECK(results, r.reason == tisple::StopReason::kCycleLimit);
    CHECK(results, r.cycles == 30);

    // an undocumented opcode stops the run before it executes
    tisple::RunResult illegal = sim.RunBytes(0x0900, {0xEA, 0x02});
    CHECK(results, illegal.reason == tisple::StopReason::kIllegalOpcode);
    CHECK(results, illegal.regs.pc == 0x0901);

    // stop address
    std::vector<uint8_t> nops = {0xEA, 0xEA, 0xEA};
    tisple::LoadBytes(*sim.machine, 0x0A00, nops.data(), nops.size());
    regs.pc = 0x0A00;
    tisple::SetRegs(*sim.machine, regs);
    tisple::RunOptions stop;
    stop.stop_pc = 0x0A02;
    tisple::RunResult at_stop = tisple::Run(*sim.machine, stop);
    CHECK(results, at_stop.reason == tisple::StopReason::kStopAddress);
    CHECK(results, at_stop.cycles == 4);
}

struct HookLog {
    std::vector<tisple::BusCycle> seen;
};

uint8_t HookRead(void* ctx, uint16_t addr) {
    static_cast<HookLog*>(ctx)->seen.push_back({addr, 0, false});
    return 0x5A;
}

void HookWrite(void* ctx, uint16_t addr, uint8_t value) {
    static_cast<HookLog*>(ctx)->seen.push_back({addr, value, true});
}

// the dummy reads and RMW double write must reach I/O hooks like they do on hardware
void TestIoHookSeesDummyAccesses(TestResults& results) {
    Sim sim;
    HookLog log;
    tisple::IoHook hook;
    hook.base = 0xC000;
    hook.size = 0x1000;
    hook.read = HookRead;
    hook.write = HookWrite;
    hook.ctx = &log;
    tisple::SetIoHook(*sim.machine, hook);

    // LDX #$20 ; LDA $C0F0,X crosses a page: reads $C010 first, then $C110
    sim.RunBytes(0x0800, {0xA2, 0x20, 0xBD, 0xF0, 0xC0, 0x00});
    CHECK(results, sim.machine->cpu.A == 0x5A);
    CHECK(results, log.seen.size() == 2);
    CHECK(results, log.seen.size() == 2 && log.seen[0].addr == 0xC010);
    CHECK(results, log.seen.size() == 2 && log.seen[1].addr == 0xC110);

    // INC $C030: read, write old value, write new value
    log.seen.clear();
    sim.RunBytes(0x0900, {0xEE, 0x30, 0xC0, 0x00});
    CHECK(results, log.seen.size() == 3);
    CHECK(results, log.seen.size() == 3 && !log.seen[0].is_write);
    CHECK(results, log.seen.size() == 3 && log.seen[1].is_write && log.seen[1].value == 0x5A);
    CHECK(results, log.seen.size() == 3 && log.seen[2].is_write && log.seen[2].value == 0x5B);
}

void TestDisassembler(TestResults& results) {
    Sim sim;
    int documented = 0;
    for (int opcode = 0; opcode < 256; opcode++) {
        documented += tisple::IsDocumentedOpcode(static_cast<uint8_t>(opcode)) ? 1 : 0;
    }
    CHECK(results, documented == 151);

    sim.machine->mem[0x0800] = 0xA9;
    sim.machine->mem[0x0801] = 0x42;
    int length = 0;
    std::string text = tisple::Disassemble(*sim.machine, 0x0800, length);
    CHECK(results, length == 2);
    CHECK(results, text == "LDA #$42");
}

} // namespace

void RunEmuBasicTests(TestResults& results) {
    TestCountdownLoop(results);
    TestDecimalAdd(results);
    TestBranchCycles(results);
    TestStopAndLimit(results);
    TestIoHookSeesDummyAccesses(results);
    TestDisassembler(results);
}

void RunEmuTests(TestResults& results) {
    RunEmuBasicTests(results);
    RunSingleStepTests(results);
}

} // namespace tisple_tests
