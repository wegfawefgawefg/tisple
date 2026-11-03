// lib/disk's reads with a song playing: the Mockingboard's tick doing a song's work (six
// voices, notes that decay, like Catwiz's battle song) while a 12 KB table is read into aux
// memory, against the same read with the song stopped. Where a read misses a sector, the step
// after it comes a sector's time late: each step's spacing says how many were missed.

#include "disk/disk_reader.hpp"
#include "machine/mockingboard.hpp"
#include "machine/tick_log.hpp"
#include "test_disk.hpp"
#include "test_disk_read.hpp"
#include "test_mockingboard.hpp"

#include <cstdio>
#include <cstring>

namespace tisple_tests {

namespace {

constexpr uint32_t kBytes = 12288;

std::string MusicProgram(const std::vector<uint8_t>& table, bool playing) {
    return "(import disk)\n"
           "(import mockingboard mb)\n"
           "(import mockingboard_song tune)\n"
           "(tune.song busy\n"
           "    (pattern bass \"l9 v12 k3 a2 - - r e2 r a2 - r g2 r r\")\n"
           "    (pattern ma \"l9 v10 k1 a4 e5 b4 r d5 c5 r e5 g4 r\")\n"
           "    (pattern mb \"l9 v9 k1 e5 r a5 g5 r e5 d5 r c5\")\n"
           "    (pattern sp \"l9 v6 k1 e6 r r b5 r a5 r\")\n"
           "    (pattern pad \"l54 v7 k12 c4 - e4 - c4 - g3 -\")\n"
           "    (pattern sh \"l9 m2 k1 n1 v5 c4 v3 c4 c4\")\n"
           "    (voice 0 bass) (voice 1 ma) (voice 2 pad) (voice 3 mb) (voice 4 sp) (voice 5 sh))\n"
           "(const BIG (array u8 12288) " +
           MakeBytes(table) +
           " :on disk)\n"
           "(const SMALL (array u8 512) " +
           MakeBytes(std::vector<uint8_t>(table.begin(), table.begin() + 512)) +
           " :on disk)\n"
           "(let room (array u8 12288) :address 0x2000 :in aux :reserve)\n"
           "(let status u8 0xEE :address 0x03C2)\n"
           "(fn irq () void :interrupt\n"
           "    (mb.service))\n"
           "(fn step () void\n"
           "    (wait_cycles 1500))\n" // a step's work, as Catwiz's paws
           "(fn main () void\n"
           "    (mb.init)\n" +
           (playing ? "    (mb.play (addr_of (at busy 0)))\n" : "") +
           "    (disk.read (as (ptr u8) 0x2000) (addr_of (at BIG 0)) disk.AUX) ; the head there\n"
           "    (set status (disk.read_with (as (ptr u8) 0x2000) (addr_of (at BIG 0)) disk.AUX "
           "step))\n"
           "    (for k u8 0 24\n"
           "        (disk.read_with (as (ptr u8) 0x5000) (addr_of (at SMALL 0)) disk.AUX step))\n"
           "    (mb.off))\n";
}

struct Steps {
    uint16_t step = 0;
    uint16_t begin = 0;
    std::vector<uint64_t> begins;
    std::vector<uint64_t> at;
    uint64_t end = 0;
    tisple::TickLog* ticks = nullptr;
};

void WatchSteps(void* ctx, const tisple::Machine& m) {
    Steps& s = *static_cast<Steps*>(ctx);
    uint16_t pc = tisple::GetRegs(m).pc;
    if (pc == s.begin) {
        s.begins.push_back(m.cycles);
    } else if (pc == s.step) {
        s.at.push_back(m.cycles);
    }
    s.end = m.cycles; // the last instruction's, once the program has returned
    tisple::TickLogStep(s.ticks, m);
}

struct Read {
    bool ok = false;
    uint64_t cycles = 0;       // the 12 KB read
    uint64_t small_cycles = 0; // the 24 reads of 512 bytes
    int late_steps = 0; // steps more than a sector's time later than two sectors after the last
    uint64_t ticks = 0;
    uint64_t latest = 0;
};

Read RunRead(TestResults& results, const std::vector<uint8_t>& table, bool playing) {
    Read r;
    tisple::BuildOptions options = DiskReadLayout(0x6000);
    options.limit = 0x6400;
    options.low_start = 0x6000;
    DiskProgram p;
    BuildDiskProgram(results, MusicProgram(table, playing), options, p);
    if (!p.ok) {
        return r;
    }
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    tisple::InsertMockingboard(*a, 4);
    tisple::TickLog log;
    tisple::StartTickLog(log, *a);
    Steps s;
    s.step = DiskProgramPointer(p, "t.step");
    s.begin = 0x1700;
    s.ticks = &log;
    uint64_t start = tisple::Cycles(*a);
    while (!a->stopped && tisple::Cycles(*a) - start < kDiskReadBudget) {
        tisple::RunApple2Traced(*a, tisple::kCyclesPerFrame, WatchSteps, &s);
    }
    r.ok = a->stop_reason == tisple::MachineStop::kReturned &&
           a->cpu.mem[0x03C2] == tisple::kReaderDone &&
           std::memcmp(a->aux + 0x2000, table.data(), table.size()) == 0 && s.begins.size() == 26;
    if (!r.ok) {
        return r;
    }
    r.cycles = s.begins[2] - s.begins[1];
    r.small_cycles = s.end - s.begins[2];
    for (size_t i = 1; i < s.at.size(); i++) {
        r.late_steps += s.at[i] - s.at[i - 1] > 25312 + 6000 ? 1 : 0;
    }
    for (const tisple::TickEntry& e : log.ticks) {
        if (e.taken > s.begins[1]) {
            r.ticks++;
            r.latest = e.late > r.latest ? e.late : r.latest;
        }
    }
    return r;
}

} // namespace

void RunDiskReadMusicTests(TestResults& results) {
    std::vector<uint8_t> table = DiskPattern(kBytes, 23);
    Read quiet = RunRead(results, table, false);
    Read song = RunRead(results, table, true);
    CHECK(results, quiet.ok && song.ok);
    std::printf("reads, a step of 1,500 cycles: song stopped 12 KB %.3f s, 24 x 512 bytes %.3f s "
                "(%d steps late); song playing %.3f s, %.3f s (%d steps late, %llu ticks, the "
                "latest %llu cycles late)\n",
                static_cast<double>(quiet.cycles) / 1023000.0,
                static_cast<double>(quiet.small_cycles) / 1023000.0, quiet.late_steps,
                static_cast<double>(song.cycles) / 1023000.0,
                static_cast<double>(song.small_cycles) / 1023000.0, song.late_steps,
                static_cast<unsigned long long>(song.ticks),
                static_cast<unsigned long long>(song.latest));
}

} // namespace tisple_tests
