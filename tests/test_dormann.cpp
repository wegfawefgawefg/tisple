// Klaus Dormann's functional tests (scripts/fetch_dormann.sh puts them in tests/data/dormann):
// each 64 KB image runs from $0400 until an instruction jumps to itself. The functional tests
// pass at their final `jmp *` (NAME.success); the decimal tests leave ERROR ($000B) 0.
// - 6502_functional_test on both cores
// - 65C02_extended_opcodes_test (the undefined opcodes as 1-byte NOPs) on the 65C02
// - the decimal test, every operand pair and carry, on each core with its own flag rules

#include "emu/machine.hpp"
#include "test_emu.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace tisple_tests {

namespace {

constexpr uint64_t kMaxCycles = 400'000'000;

bool LoadImage(const std::string& path, tisple::Machine& m) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    size_t got = std::fread(m.mem, 1, sizeof(m.mem), file);
    std::fclose(file);
    return got == sizeof(m.mem);
}

int ReadSuccess(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) {
        return -1;
    }
    unsigned value = 0;
    int read = std::fscanf(file, "%x", &value);
    std::fclose(file);
    return read == 1 ? static_cast<int>(value) : -1;
}

struct TrapResult {
    uint16_t pc = 0;
    uint64_t cycles = 0;
    bool trapped = false;
};

// runs until an instruction leaves pc where it was
TrapResult RunToTrap(tisple::Machine& m) {
    TrapResult r;
    for (;;) {
        uint16_t pc = M6502_GET_ADDR(m.pins);
        tisple::StepInstruction(m);
        if (M6502_GET_ADDR(m.pins) == pc) {
            r.pc = pc;
            r.trapped = true;
            break;
        }
        if (m.cycles >= kMaxCycles) {
            r.pc = pc;
            break;
        }
    }
    r.cycles = m.cycles;
    return r;
}

// one image on one core; false when the data is missing
bool RunImage(TestResults& results, const char* name, tisple::Cpu cpu, bool decimal) {
    std::string base = std::string("tests/data/dormann/") + name;
    auto m = std::make_unique<tisple::Machine>();
    tisple::InitMachine(*m, cpu);
    if (!LoadImage(base + ".bin", *m)) {
        return false;
    }
    tisple::Regs regs;
    regs.pc = 0x0400;
    regs.s = 0xFF;
    tisple::SetRegs(*m, regs);
    TrapResult r = RunToTrap(*m);
    bool ok = false;
    if (decimal) {
        ok = r.trapped && m->mem[0x000B] == 0;
    } else {
        ok = r.trapped && static_cast<int>(r.pc) == ReadSuccess(base + ".success");
    }
    CHECK(results, ok);
    std::printf("dormann %s on the %s: %s at $%04X after %llu cycles\n", name, tisple::CpuName(cpu),
                ok ? "passed" : "FAILED", r.pc, static_cast<unsigned long long>(r.cycles));
    return true;
}

} // namespace

void RunDormannTests(TestResults& results) {
    bool any = RunImage(results, "6502_functional_test", tisple::Cpu::k6502, false);
    if (!any) {
        std::printf("dormann: skipped (no data; run scripts/fetch_dormann.sh)\n");
        return;
    }
    RunImage(results, "6502_functional_test", tisple::Cpu::k65c02, false);
    RunImage(results, "65C02_extended_opcodes_test", tisple::Cpu::k65c02, false);
    RunImage(results, "6502_decimal_test_nmos", tisple::Cpu::k6502, true);
    RunImage(results, "6502_decimal_test_65c02", tisple::Cpu::k65c02, true);
}

} // namespace tisple_tests
