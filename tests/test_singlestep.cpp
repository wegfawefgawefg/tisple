// checks the simulator against SingleStepTests/65x02 (data fetched by
// scripts/fetch_singlestep.sh): final registers, final memory and every bus cycle

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
const char* RunCase(tisple::Machine& m, const StepCase& c, std::vector<tisple::BusCycle>& log) {
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
        if (a.addr != b.addr || a.value != b.value || a.is_write != b.is_write) {
            problem = "bus cycle";
            std::fprintf(stderr, "  cycle %zu got %04x %02x %d want %04x %02x %d\n", i, a.addr,
                         a.value, a.is_write, b.addr, b.value, b.is_write);
        }
    }
    if (problem == nullptr && (got.pc != want.pc || got.a != want.a || got.x != want.x ||
                               got.y != want.y || got.s != want.s || got.p != want.p)) {
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

} // namespace

void RunSingleStepTests(TestResults& results) {
    auto machine = std::make_unique<tisple::Machine>();
    tisple::InitMachine(*machine);
    std::vector<tisple::BusCycle> log;
    tisple::SetBusLog(*machine, &log);

    int files = 0;
    int total_cases = 0;
    for (int opcode = 0; opcode < 256; opcode++) {
        char path[128];
        std::snprintf(path, sizeof(path), "tests/data/singlestep/%02x.bin", opcode);
        std::vector<StepCase> cases;
        if (!ReadCases(path, cases)) {
            continue;
        }
        files++;
        int passed = 0;
        int failed = 0;
        for (const StepCase& c : cases) {
            const char* problem = RunCase(*machine, c, log);
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
            std::printf("singlestep %02x: %d passed, %d failed\n", opcode, passed, failed);
        }
    }
    if (files == 0) {
        std::printf("singlestep: skipped (no data; run scripts/fetch_singlestep.sh)\n");
        return;
    }
    std::printf("singlestep: %d opcode files, %d cases\n", files, total_cases);
}

} // namespace tisple_tests
