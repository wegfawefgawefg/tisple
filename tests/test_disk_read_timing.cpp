// lib/disk's speed and the time it leaves between sectors, measured on the built-in IIe: an
// 8 KB table laid out 1:1, 2:1 and 3:1, read with a step function that waits longer and
// longer, its calls counted and timed; and the Mockingboard's ticks during a read, late while
// a sector is read but none lost. Prints what docs/disks.md quotes.

#include "disk/disk_reader.hpp"
#include "machine/disk2.hpp"
#include "machine/mockingboard.hpp"
#include "test_disk.hpp"
#include "test_disk_read.hpp"
#include "test_mockingboard.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tisple_tests {

namespace {

constexpr uint32_t kTableBytes = 8192;
constexpr int kPieces = kTableBytes / 256;
constexpr uint16_t kBegin = 0x1700; // lib/disk's BEGIN
const int kDelays[] = {0, 4000, 8000, 8500, 8750, 9000, 12000, 16000, 20000, 21000, 22000, 30000};
constexpr int kDelayCount = static_cast<int>(sizeof(kDelays) / sizeof(kDelays[0]));
constexpr int kLayouts = 3; // interleave 1, 2, 3

std::string TimingProgram(const std::vector<uint8_t>& table) {
    std::string bytes = MakeBytes(table);
    std::string text = "(import disk)\n";
    for (int n = 1; n <= kLayouts; n++) {
        text += "(const T" + std::to_string(n) + " (array u8 8192) " + bytes +
                " :on disk :interleave " + std::to_string(n) + ")\n";
    }
    text += "(const TABLES (array (ptr u8) 3) (make (array (ptr u8) 3) (addr_of (at T1 0)) "
            "(addr_of (at T2 0)) (addr_of (at T3 0))))\n";
    text += "(const DELAYS (array u16 " + std::to_string(kDelayCount) + ") (make (array u16 " +
            std::to_string(kDelayCount) + ")";
    for (int d : kDelays) {
        text += " " + std::to_string(d);
    }
    text += "))\n";
    text += "(let room (array u8 8192) :address 0x2000 :reserve)\n"
            "(let delay u16 0)\n"
            "(let count u8 0)\n"
            "(let counts (array u8 64))\n"
            "(let statuses (array u8 64))\n"
            "(fn step () void\n"
            "    (set count (+ count 1))\n"
            "    (if (> delay 0)\n"
            "        (wait_cycles delay)))\n"
            "(fn main () void\n"
            "    (disk.read (as (ptr u8) 0x2000) (addr_of (at T1 0)) disk.MAIN) ; the head there\n"
            "    (for t u8 0 3\n"
            "        (for k u8 0 " +
            std::to_string(kDelayCount) +
            "\n"
            "            (set delay (at DELAYS k))\n"
            "            (set count 0)\n"
            "            (let n u8 (+ (* t " +
            std::to_string(kDelayCount) +
            ") k))\n"
            "            (set (at statuses n) (disk.read_with (as (ptr u8) 0x2000) "
            "(at TABLES t) disk.MAIN step))\n"
            "            (set (at counts n) count))))\n";
    return text;
}

// when each read began (BEGIN's first instruction) and when step was called
struct Times {
    uint16_t step = 0;
    std::vector<uint64_t> begins;
    std::vector<std::vector<uint64_t>> steps;
};

void WatchTimes(void* ctx, const tisple::Machine& m) {
    Times& t = *static_cast<Times*>(ctx);
    uint16_t pc = tisple::GetRegs(m).pc;
    if (pc == kBegin) {
        t.begins.push_back(m.cycles);
        t.steps.emplace_back();
    } else if (pc == t.step && !t.steps.empty()) {
        t.steps.back().push_back(m.cycles);
    }
}

double KBs(uint64_t cycles) {
    return static_cast<double>(kTableBytes) / 1024.0 / (static_cast<double>(cycles) / 1023000.0);
}

void TestBudget(TestResults& results) {
    std::vector<uint8_t> table = DiskPattern(kTableBytes, 5);
    DiskProgram p;
    BuildDiskProgram(results, TimingProgram(table), DiskReadLayout(0x6000), p);
    if (!p.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    Times times;
    times.step = DiskProgramPointer(p, "t.step");
    uint64_t start = tisple::Cycles(*a);
    while (!a->stopped && tisple::Cycles(*a) - start < 4 * kDiskReadBudget) {
        tisple::RunApple2Traced(*a, tisple::kCyclesPerFrame, WatchTimes, &times);
    }
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, times.step != 0);
    int reads = kLayouts * kDelayCount;
    CHECK(results, static_cast<int>(times.begins.size()) == reads + 1);
    if (static_cast<int>(times.begins.size()) != reads + 1) {
        return;
    }
    times.begins.erase(times.begins.begin()); // the first read, which found the head
    times.steps.erase(times.steps.begin());
    times.begins.push_back(tisple::Cycles(*a));
    uint16_t counts = DiskProgramVar(p, "t.counts");
    uint16_t statuses = DiskProgramVar(p, "t.statuses");
    bool all = true;
    for (int n = 0; n < reads; n++) {
        all = all && a->cpu.mem[statuses + n] == tisple::kReaderDone &&
              a->cpu.mem[counts + n] == kPieces - 1 &&
              static_cast<int>(times.steps[static_cast<size_t>(n)].size()) == kPieces - 1;
    }
    CHECK(results, all);
    CHECK(results, std::memcmp(a->cpu.mem + 0x2000, table.data(), table.size()) == 0);
    // the read times: from BEGIN to the next BEGIN (the loop around is a few dozen cycles)
    std::printf("disk read timing, 8 KB (2 tracks), seconds by the step's wait:\n");
    std::printf("  wait     1:1    2:1    3:1\n");
    uint64_t took[kLayouts][kDelayCount];
    for (int k = 0; k < kDelayCount; k++) {
        std::printf("  %5d", kDelays[k]);
        for (int t = 0; t < kLayouts; t++) {
            size_t n = static_cast<size_t>(t * kDelayCount + k);
            took[t][k] = times.begins[n + 1] - times.begins[n];
            std::printf("  %5.2f", static_cast<double>(took[t][k]) / 1023000.0);
        }
        std::printf("\n");
    }
    // the gap: the longest wait that costs at most a twentieth more than none
    int budget[kLayouts] = {0, 0, 0};
    for (int t = 0; t < kLayouts; t++) {
        for (int k = 0; k < kDelayCount && took[t][k] * 20 <= took[t][0] * 21; k++) {
            budget[t] = kDelays[k];
        }
    }
    // the steps' spacing with no wait, 2:1: two sectors' time, a sector's on the next track
    const std::vector<uint64_t>& steps = times.steps[static_cast<size_t>(kDelayCount)];
    uint64_t least = UINT64_MAX;
    for (size_t i = 1; i < steps.size(); i++) {
        least = std::min(least, steps[i] - steps[i - 1]);
    }
    std::printf("  KB/s with no wait: 1:1 %.1f, 2:1 %.1f, 3:1 %.1f; a wait up to %d (1:1), %d "
                "(2:1), %d (3:1) cycles costs under 5%%; steps at 2:1 at least %llu cycles "
                "apart\n",
                KBs(took[0][0]), KBs(took[1][0]), KBs(took[2][0]), budget[0], budget[1], budget[2],
                static_cast<unsigned long long>(least));
    // two turns a track and a seek: at least 8 KB a second at 1:1 and 2:1
    CHECK(results, KBs(took[0][0]) > 8.0 && KBs(took[1][0]) > 8.0);
    CHECK(results, budget[1] >= 8000);
    CHECK(results, least > 24000);
}

std::string IrqProgram(const std::vector<uint8_t>& table) {
    return "(import disk)\n"
           "(import mockingboard mb)\n"
           "(const BIG (array u8 12288) " +
           MakeBytes(table) +
           " :on disk)\n"
           "(let room (array u8 12288) :address 0x2000 :in aux :reserve)\n"
           "(let ticks u16 0 :address 0x03C0)\n"
           "(let status u8 0xEE :address 0x03C2)\n"
           "(let steps u16 0 :address 0x03C4)\n"
           "(fn irq () void :interrupt\n"
           "    (mb.service)\n"
           "    (set ticks (+ ticks 1)))\n"
           "(fn step () void\n"
           "    (set steps (+ steps 1)))\n"
           "(fn main () void\n"
           "    (mb.init)\n"
           "    (set status (disk.read_with (as (ptr u8) 0x2000) (addr_of (at BIG 0)) disk.AUX "
           "step))\n"
           "    (mb.off))\n";
}

struct Ticks {
    uint16_t entry = 0;
    std::vector<uint64_t> at;
};

void WatchTicks(void* ctx, const tisple::Machine& m) {
    Ticks& t = *static_cast<Ticks*>(ctx);
    if (tisple::GetRegs(m).pc == t.entry) {
        t.at.push_back(m.cycles);
    }
}

// the Mockingboard ticking through a 12 KB read into aux memory: every tick taken, late by at
// most a sector's read
void TestTicks(TestResults& results) {
    std::vector<uint8_t> table = DiskPattern(12288, 17);
    // main memory small enough that the program goes in the language card, whose ram vector
    // takes the interrupts (no Apple rom here)
    tisple::BuildOptions options = DiskReadLayout(0x6000);
    options.limit = 0x6400;
    options.low_start = 0x6000;
    DiskProgram p;
    BuildDiskProgram(results, IrqProgram(table), options, p);
    if (!p.ok) {
        return;
    }
    CHECK(results, p.code.card_ram && p.code.irq_direct != 0);
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    tisple::InsertMockingboard(*a, 4);
    Ticks ticks;
    ticks.entry = p.code.irq_direct;
    uint64_t start = tisple::Cycles(*a);
    while (!a->stopped && tisple::Cycles(*a) - start < kDiskReadBudget) {
        tisple::RunApple2Traced(*a, tisple::kCyclesPerFrame, WatchTicks, &ticks);
    }
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, a->cpu.mem[0x03C2] == tisple::kReaderDone);
    CHECK(results, (a->cpu.mem[0x03C4] | (a->cpu.mem[0x03C5] << 8)) == 12288 / 256 - 1);
    CHECK(results, std::memcmp(a->aux + 0x2000, table.data(), table.size()) == 0);
    uint64_t counted = a->cpu.mem[0x03C0] | (a->cpu.mem[0x03C1] << 8);
    CHECK(results, counted == ticks.at.size() && counted > 60);
    if (ticks.at.size() < 2) {
        return;
    }
    // ticks come every kMbTick cycles; each one's lateness from the earliest-landing phase
    // a tick lost would put every later one a whole tick late
    uint64_t span = ticks.at.back() - ticks.at.front();
    int64_t phase = INT64_MAX;
    for (size_t i = 0; i < ticks.at.size(); i++) {
        int64_t at = static_cast<int64_t>(ticks.at[i]) - static_cast<int64_t>(i * kMbTick);
        phase = std::min(phase, at);
    }
    int64_t latest = 0;
    for (size_t i = 0; i < ticks.at.size(); i++) {
        int64_t late =
            static_cast<int64_t>(ticks.at[i]) - static_cast<int64_t>(i * kMbTick) - phase;
        latest = std::max(latest, late);
    }
    CHECK(results, latest < 14000);
    std::printf("disk read under the Mockingboard: %llu ticks over %.2f s, none lost, the latest "
                "%lld cycles late\n",
                static_cast<unsigned long long>(counted), static_cast<double>(span) / 1023000.0,
                static_cast<long long>(latest));
}

} // namespace

void RunDiskReadTimingTests(TestResults& results) {
    TestBudget(results);
    TestTicks(results);
}

} // namespace tisple_tests
