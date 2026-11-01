// checks the simulator against SingleStepTests/65x02 (data fetched by
// scripts/fetch_singlestep.sh): final registers, final memory and every bus cycle.
// - the NMOS core: the 151 documented opcodes of the 6502 set
// - the 65C02 core: all 256 opcodes of the synertek65c02 set (the 65C02 without Rockwell's bit
//   instructions) but 35. That set's chip treats $x7, $xF, $CB, $DB and $5C as its own NOPs
//   ($07 a 2-byte zero-page read, $5C 4 cycles, ...); ours are the documented ones (Bruce
//   Clark's table: 1 byte 1 cycle, $5C 8 cycles), which Dormann's 65C02 test checks instead.
//   And ADC # and SBC # in decimal mode: its extra cycle reads a fixed $0056 or $0000, an
//   artefact of the generating emulator; ours reads the operand again, as every other mode
//   does in that set. Everything but that one cycle is compared.
// B is not a flag the chip keeps (only pushes), so P is compared without it: the 65C02 set's
// SBC files have it set in every state, the others never.

#include "emu/harness.hpp"
#include "test_emu.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace tisple_tests {

namespace {

struct RamEntry {
    uint16_t addr = 0;
    uint8_t value = 0;
};

struct StepState {
    tisple::Regs regs;
    std::vector<RamEntry> ram;
};

struct StepCase {
    StepState initial;
    StepState final_state;
    std::vector<tisple::BusCycle> cycles;
};

struct Reader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    bool ok = true;

    uint32_t Take(int bytes) {
        if (pos + static_cast<size_t>(bytes) > size) {
            ok = false;
            return 0;
        }
        uint32_t value = 0;
        for (int i = 0; i < bytes; i++) {
            value |= static_cast<uint32_t>(data[pos + static_cast<size_t>(i)]) << (8 * i);
        }
        pos += static_cast<size_t>(bytes);
        return value;
    }
};

StepState ReadState(Reader& r) {
    StepState state;
    state.regs.pc = static_cast<uint16_t>(r.Take(2));
    state.regs.s = static_cast<uint8_t>(r.Take(1));
    state.regs.a = static_cast<uint8_t>(r.Take(1));
    state.regs.x = static_cast<uint8_t>(r.Take(1));
    state.regs.y = static_cast<uint8_t>(r.Take(1));
    state.regs.p = static_cast<uint8_t>(r.Take(1));
    uint32_t count = r.Take(2);
    for (uint32_t i = 0; i < count && r.ok; i++) {
        RamEntry entry;
        entry.addr = static_cast<uint16_t>(r.Take(2));
        entry.value = static_cast<uint8_t>(r.Take(1));
        state.ram.push_back(entry);
    }
    return state;
}

bool ReadCases(const char* path, std::vector<StepCase>& cases) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + got);
    }
    std::fclose(file);

    Reader r;
    r.data = bytes.data();
    r.size = bytes.size();
    uint32_t count = r.Take(4);
    for (uint32_t i = 0; i < count && r.ok; i++) {
        StepCase c;
        c.initial = ReadState(r);
        c.final_state = ReadState(r);
        uint32_t cycle_count = r.Take(1);
        for (uint32_t j = 0; j < cycle_count && r.ok; j++) {
            tisple::BusCycle cycle;
            cycle.addr = static_cast<uint16_t>(r.Take(2));
            cycle.value = static_cast<uint8_t>(r.Take(1));
            cycle.is_write = r.Take(1) != 0;
            c.cycles.push_back(cycle);
        }
        cases.push_back(std::move(c));
    }
    return r.ok;
}

// first difference between what the simulator did and the expected case, or null
// loose_last: the last bus cycle's address is not compared (decimal ADC # and SBC #)
const char* RunCase(tisple::Machine& m, const StepCase& c, std::vector<tisple::BusCycle>& log,
                    bool loose_last) {
    for (const RamEntry& e : c.initial.ram) {
        m.mem[e.addr] = e.value;
    }
    tisple::SetRegs(m, c.initial.regs);
    log.clear();
    m.cycles = 0;
    tisple::StepInstruction(m);

    const char* problem = nullptr;
    tisple::Regs got = tisple::GetRegs(m);
    const tisple::Regs& want = c.final_state.regs;
    if (log.size() != c.cycles.size() || m.cycles != c.cycles.size()) {
        problem = "cycle count";
    }
    for (size_t i = 0; problem == nullptr && i < log.size(); i++) {
        const tisple::BusCycle& a = log[i];
        const tisple::BusCycle& b = c.cycles[i];
        bool loose = loose_last && i + 1 == log.size();
        if ((a.addr != b.addr || a.value != b.value || a.is_write != b.is_write) &&
            !(loose && a.is_write == b.is_write)) {
            problem = "bus cycle";
            std::fprintf(stderr, "  cycle %zu got %04x %02x %d want %04x %02x %d\n", i, a.addr,
                         a.value, a.is_write, b.addr, b.value, b.is_write);
        }
    }
    if (problem == nullptr &&
        (got.pc != want.pc || got.a != want.a || got.x != want.x || got.y != want.y ||
         got.s != want.s || (got.p | tisple::kFlagB) != (want.p | tisple::kFlagB))) {
        problem = "registers";
        std::fprintf(stderr, "  got pc=%04x a=%02x x=%02x y=%02x s=%02x p=%02x\n", got.pc, got.a,
                     got.x, got.y, got.s, got.p);
        std::fprintf(stderr, " want pc=%04x a=%02x x=%02x y=%02x s=%02x p=%02x\n", want.pc, want.a,
                     want.x, want.y, want.s, want.p);
    }
    for (size_t i = 0; problem == nullptr && i < c.final_state.ram.size(); i++) {
        const RamEntry& e = c.final_state.ram[i];
        if (m.mem[e.addr] != e.value) {
            problem = "memory";
        }
    }
    // leave memory clean for the next case
    for (const RamEntry& e : c.initial.ram) {
        m.mem[e.addr] = 0;
    }
    for (const RamEntry& e : c.final_state.ram) {
        m.mem[e.addr] = 0;
    }
    return problem;
}

void PrintMismatch(int opcode, const StepCase& c, const char* problem) {
    std::fprintf(stderr,
                 "singlestep %02x: %s mismatch, pc=%04x a=%02x x=%02x y=%02x s=%02x p=%02x\n",
                 opcode, problem, c.initial.regs.pc, c.initial.regs.a, c.initial.regs.x,
                 c.initial.regs.y, c.initial.regs.s, c.initial.regs.p);
}

// the 65C02 opcodes whose synertek65c02 cases are not ours (above)
bool SkipsSynertek(int opcode) {
    int low = opcode & 0x0F;
    return low == 0x07 || low == 0x0F || opcode == 0xCB || opcode == 0xDB || opcode == 0x5C;
}

// one directory of opcode files on one core; false when it has no data
bool RunSet(TestResults& results, const char* dir, tisple::Cpu cpu) {
    auto machine = std::make_unique<tisple::Machine>();
    tisple::InitMachine(*machine, cpu);
    std::vector<tisple::BusCycle> log;
    tisple::SetBusLog(*machine, &log);

    int files = 0;
    int total_cases = 0;
    for (int opcode = 0; opcode < 256; opcode++) {
        if (cpu == tisple::Cpu::k65c02 && SkipsSynertek(opcode)) {
            continue;
        }
        char path[128];
        std::snprintf(path, sizeof(path), "%s/%02x.bin", dir, opcode);
        std::vector<StepCase> cases;
        if (!ReadCases(path, cases)) {
            continue;
        }
        files++;
        int passed = 0;
        int failed = 0;
        for (const StepCase& c : cases) {
            bool loose = cpu == tisple::Cpu::k65c02 && (opcode == 0x69 || opcode == 0xE9) &&
                         (c.initial.regs.p & tisple::kFlagD) != 0;
            const char* problem = RunCase(*machine, c, log, loose);
            if (problem == nullptr) {
                passed++;
                continue;
            }
            if (failed++ < 2) {
                PrintMismatch(opcode, c, problem);
            }
        }
        total_cases += passed + failed;
        results.passed += passed;
        results.failed += failed;
        if (std::getenv("TISPLE_TEST_VERBOSE") != nullptr || failed != 0) {
            std::printf("singlestep %s %02x: %d passed, %d failed\n", tisple::CpuName(cpu), opcode,
                        passed, failed);
        }
    }
    if (files == 0) {
        return false;
    }
    std::printf("singlestep %s: %d opcode files, %d cases\n", tisple::CpuName(cpu), files,
                total_cases);
    return true;
}

} // namespace

void RunSingleStepTests(TestResults& results) {
    if (!RunSet(results, "tests/data/singlestep", tisple::Cpu::k6502)) {
        std::printf("singlestep: skipped (no data; run scripts/fetch_singlestep.sh)\n");
    }
    if (!RunSet(results, "tests/data/singlestep65c02", tisple::Cpu::k65c02)) {
        std::printf("singlestep 65c02: skipped (no data; run scripts/fetch_singlestep.sh 65c02)\n");
    }
}

} // namespace tisple_tests
