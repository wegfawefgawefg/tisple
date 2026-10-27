// tisple race's timing: inputs written at the kernel's entry, cycles from entry to return

#include "test_race.hpp"

#include "race/race.hpp"

#include <memory>
#include <vector>

namespace tisple_tests {

namespace {

struct RaceSim {
    std::unique_ptr<tisple::Machine> machine = std::make_unique<tisple::Machine>();

    RaceSim() {
        tisple::InitMachine(*machine);
    }

    void Load(uint16_t addr, std::vector<uint8_t> bytes) {
        tisple::LoadBytes(*machine, addr, bytes.data(), bytes.size());
    }
};

void TestKernelTimedFromEntryToReturn(TestResults& results) {
    RaceSim sim;
    // startup: LDA #$00 ; STA $0900 ; JSR kernel ; BRK
    sim.Load(0x0800, {0xA9, 0x00, 0x8D, 0x00, 0x09, 0x20, 0x10, 0x08, 0x00});
    // kernel: LDA $0900 ; STA $0901 ; RTS
    sim.Load(0x0810, {0xAD, 0x00, 0x09, 0x8D, 0x01, 0x09, 0x60});
    tisple::RaceOptions options;
    options.start = 0x0800;
    options.entry = 0x0810;
    // the startup's store to $0900 runs first, then the patch replaces it
    options.patches.push_back({0x0900, {0x42}});
    tisple::RaceResult r = tisple::RunRace(*sim.machine, options);
    CHECK(results, r.returned);
    CHECK(results, r.startup_cycles == 2 + 4 + 6);
    CHECK(results, r.kernel_cycles == 4 + 4 + 6);
    CHECK(results, r.regs.pc == 0x0808);
    CHECK(results, sim.machine->mem[0x0901] == 0x42);
    CHECK(results, r.executed[0x0810] == 1 && r.executed[0x0816] == 1);
    CHECK(results, r.executed[0x0800] == 0);
    CHECK(results, tisple::ExecutedRanges(*sim.machine, r.executed) == "0810 0816\n");
}

void TestRecursiveReturnIsNotTheEnd(TestResults& results) {
    RaceSim sim;
    // startup: JSR kernel ; BRK
    sim.Load(0x0800, {0x20, 0x10, 0x08, 0x00});
    // kernel: DEC $0900 ; BEQ done ; JSR kernel ; done: RTS. The inner returns land on
    // the RTS with the stack deeper than at entry, so only the outermost one counts.
    sim.Load(0x0810, {0xCE, 0x00, 0x09, 0xF0, 0x03, 0x20, 0x10, 0x08, 0x60});
    tisple::RaceOptions options;
    options.start = 0x0800;
    options.entry = 0x0810;
    options.patches.push_back({0x0900, {3}});
    tisple::RaceResult r = tisple::RunRace(*sim.machine, options);
    CHECK(results, r.returned);
    CHECK(results, r.regs.pc == 0x0803);
    CHECK(results, sim.machine->mem[0x0900] == 0);
    // two calls go deeper (6 + 2 + 6 each), the last DEC finds 0 (6 + 3), three RTS
    CHECK(results, r.kernel_cycles == 2 * (6 + 2 + 6) + (6 + 3) + 3 * 6);
}

// the random-program race stops the clock at t.checksum and lets the kernel finish
void TestClockStopsAtTheStopAddress(TestResults& results) {
    RaceSim sim;
    sim.Load(0x0800, {0x20, 0x10, 0x08, 0x00}); // JSR kernel ; BRK
    // kernel: LDA $0900 ; JSR stop ; RTS
    sim.Load(0x0810, {0xAD, 0x00, 0x09, 0x20, 0x20, 0x08, 0x60});
    // stop: STA $0901 ; RTS
    sim.Load(0x0820, {0x8D, 0x01, 0x09, 0x60});
    tisple::RaceOptions options;
    options.start = 0x0800;
    options.entry = 0x0810;
    options.has_stop = true;
    options.stop = 0x0820;
    options.patches.push_back({0x0900, {0x42}});
    tisple::RaceResult r = tisple::RunRace(*sim.machine, options);
    CHECK(results, r.returned);
    CHECK(results, r.kernel_cycles == 4 + 6);
    CHECK(results, r.total_cycles == 4 + 6 + 4 + 6 + 6);
    CHECK(results, sim.machine->mem[0x0901] == 0x42);
    CHECK(results, tisple::ExecutedRanges(*sim.machine, r.executed) == "0810 0815\n");

    // a kernel that never gets there is an error
    options.stop = 0x0830;
    RaceSim again;
    again.Load(0x0800, {0x20, 0x10, 0x08, 0x00});
    again.Load(0x0810, {0xAD, 0x00, 0x09, 0x20, 0x20, 0x08, 0x60});
    again.Load(0x0820, {0x8D, 0x01, 0x09, 0x60});
    r = tisple::RunRace(*again.machine, options);
    CHECK(results, !r.returned);
    CHECK(results, r.error == "the kernel returned without reaching the stop address");
}

// and starts it when t.init returns
void TestClockStartsAfterACall(TestResults& results) {
    RaceSim sim;
    sim.Load(0x0800, {0x20, 0x10, 0x08, 0x00}); // JSR kernel ; BRK
    // kernel: JSR init ; LDA $0900 ; JSR stop ; RTS
    sim.Load(0x0810, {0x20, 0x30, 0x08, 0xAD, 0x00, 0x09, 0x20, 0x20, 0x08, 0x60});
    sim.Load(0x0820, {0x8D, 0x01, 0x09, 0x60});             // stop: STA $0901 ; RTS
    sim.Load(0x0830, {0xA9, 0x07, 0x8D, 0x00, 0x09, 0x60}); // init: LDA #7 ; STA $0900 ; RTS
    tisple::RaceOptions options;
    options.start = 0x0800;
    options.entry = 0x0810;
    options.has_after = true;
    options.after = 0x0830;
    options.has_stop = true;
    options.stop = 0x0820;
    tisple::RaceResult r = tisple::RunRace(*sim.machine, options);
    CHECK(results, r.returned);
    CHECK(results, r.kernel_cycles == 4 + 6);
    CHECK(results, r.total_cycles == 6 + (2 + 4 + 6) + 4 + 6 + (4 + 6) + 6);
    CHECK(results, sim.machine->mem[0x0901] == 7);
    CHECK(results, tisple::ExecutedRanges(*sim.machine, r.executed) == "0813 0818\n");
}

void TestKernelNeverEntered(TestResults& results) {
    RaceSim sim;
    sim.Load(0x0800, {0xEA, 0x00}); // NOP ; BRK
    tisple::RaceOptions options;
    options.start = 0x0800;
    options.entry = 0x0810;
    tisple::RaceResult r = tisple::RunRace(*sim.machine, options);
    CHECK(results, !r.returned);
    CHECK(results, r.error == "BRK at $0801 before the kernel started");
}

} // namespace

void RunRaceTests(TestResults& results) {
    TestKernelTimedFromEntryToReturn(results);
    TestRecursiveReturnIsNotTheEnd(results);
    TestClockStopsAtTheStopAddress(results);
    TestClockStartsAfterACall(results);
    TestKernelNeverEntered(results);
}

} // namespace tisple_tests
