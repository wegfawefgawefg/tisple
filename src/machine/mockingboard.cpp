#include "machine/mockingboard.hpp"

#include "machine/ay8910.hpp"
#include "machine/slots.hpp"
#include "machine/via6522.hpp"

#include <cstdio>

namespace tisple {

namespace {

MockingboardState& Card(Apple2& a) {
    return a.slots.mockingboard;
}

uint64_t AccessCycle(const Apple2& a) {
    // the cpu wrapper counts the cycle before performing it
    return a.cpu.cycles - 1;
}

void Log(MockingboardState& mb, uint64_t cycle, int chip, uint8_t reg, uint8_t value) {
    PsgWrite& w = mb.log[mb.log_count % kPsgLogSize];
    w.cycle = cycle;
    w.chip = static_cast<uint8_t>(chip);
    w.reg = reg;
    w.value = value;
    mb.log_count++;
}

// the 6522's ports moved: the AY sees its lines
void DriveAy(MockingboardState& mb, int chip, uint64_t cycle) {
    const Via6522& via = mb.via[chip];
    uint8_t reg = 0;
    uint8_t value = 0;
    if (AyBus(mb.ay[chip], ViaPortB(via), ViaPortA(via), reg, value)) {
        Log(mb, cycle, chip, reg, value);
        mb.writes += reg != kPsgLogReset ? 1 : 0;
    }
}

// the cpu's IRQ line: low from the first cycle either 6522 holds it low. the cpu wrapper
// looks at the line after counting the cycle, so a line low from cycle t shows at count t + 1
void UpdateIrq(Apple2& a) {
    const MockingboardState& mb = a.slots.mockingboard;
    uint64_t first = ViaIrqCycle(mb.via[0]);
    uint64_t second = ViaIrqCycle(mb.via[1]);
    uint64_t at = first < second ? first : second;
    SetIrqLevel(a.cpu, at == kViaNever ? UINT64_MAX : at + 1);
}

bool IsPortRegister(int reg) {
    return reg <= 3 || reg == 15;
}

} // namespace

uint8_t MockingboardRomAccess(Apple2& a, int offset, bool is_write, uint8_t value, bool& drives) {
    MockingboardState& mb = Card(a);
    uint64_t cycle = AccessCycle(a);
    int chip = (offset >> 7) & 1;
    int reg = offset & 0x0F;
    mb.accesses++;
    if (!is_write) {
        drives = true;
        uint8_t got = ViaRead(mb.via[chip], reg, cycle, AyDataOut(mb.ay[chip]), 0xFF);
        UpdateIrq(a);
        return got;
    }
    drives = false;
    ViaWrite(mb.via[chip], reg, value, cycle);
    if (IsPortRegister(reg)) {
        DriveAy(mb, chip, cycle);
    }
    UpdateIrq(a);
    return 0;
}

void ResetMockingboard(Apple2& a) {
    MockingboardState& mb = Card(a);
    uint64_t cycle = a.cpu.cycles;
    for (int chip = 0; chip < 2; chip++) {
        ResetVia(mb.via[chip], cycle);
        // /RESET low, then the lines as the reset 6522 leaves them (inputs: all high)
        uint8_t reg = 0;
        uint8_t value = 0;
        AyBus(mb.ay[chip], 0x00, 0xFF, reg, value);
        Log(mb, cycle, chip, kPsgLogReset, 0);
        DriveAy(mb, chip, cycle);
    }
    UpdateIrq(a);
}

void InsertMockingboard(Apple2& a, int slot) {
    InsertCard(a, slot, CardKind::kMockingboard);
    MockingboardState& mb = Card(a);
    mb.slot = static_cast<uint8_t>(slot);
    for (int chip = 0; chip < 2; chip++) {
        mb.via[chip] = Via6522{};
        mb.ay[chip] = AyChip{};
    }
    mb.log_count = 0;
    mb.accesses = 0;
    mb.writes = 0;
    ResetMockingboard(a);
}

bool HasMockingboard(const Apple2& a) {
    const MockingboardState& mb = a.slots.mockingboard;
    return mb.slot >= 1 && mb.slot <= 7 && a.slots.kind[mb.slot] == CardKind::kMockingboard;
}

const PsgWrite& MockingboardWrite(const Apple2& a, uint64_t n) {
    return a.slots.mockingboard.log[n % kPsgLogSize];
}

std::string DescribeMockingboard(const Apple2& a) {
    const MockingboardState& mb = a.slots.mockingboard;
    if (!HasMockingboard(a) || mb.accesses == 0) {
        return "";
    }
    char text[160];
    std::snprintf(text, sizeof(text),
                  "mockingboard: slot %d, %u accesses, %llu sound chip writes, %llu "
                  "interrupts taken\n",
                  mb.slot, mb.accesses, static_cast<unsigned long long>(mb.writes),
                  static_cast<unsigned long long>(a.cpu.irqs_taken));
    return text;
}

} // namespace tisple
