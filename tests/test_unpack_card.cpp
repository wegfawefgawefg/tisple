// lib/unpack's card modes under lib/mockingboard's ticks: a double hi-res picture's half (8 KB)
// unpacked into the aux language card's bank 2, and 4 KB of few-valued bytes (short matches,
// the unpacker's slowest bytes) into its bank 1, while the card ticks 60 times a second. With
// (auxmem.guard) the unpacker lets a waiting tick in at each page out: every tick is taken, the
// latest by about a page's unpacking. Without the guard interrupts stay off throughout, as
// before: the ticks wait and all but one of those due meanwhile are lost, and nothing breaks.
// Every byte comes out right both ways. Prints what docs/dhgr.md quotes.

#include "machine/mockingboard.hpp"
#include "machine/tick_log.hpp"
#include "test_mockingboard.hpp"
#include "test_pack.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

constexpr uint16_t kTicks = 0x03C0;
constexpr size_t kBank1Bytes = 4096;

std::string Bytes(const std::vector<uint8_t>& data) {
    std::string text;
    for (uint8_t b : data) {
        text += (text.empty() ? "" : " ") + std::to_string(b);
    }
    return text;
}

std::string Program(const std::vector<uint8_t>& picture, const std::vector<uint8_t>& few,
                    bool guard) {
    return R"(
(import auxmem)
(import auxcard)
(import tables)
(import unpack)
(import mockingboard mb)
(import mockingboard_song tune)
(tune.song hum (pattern a "l1 v12 c4 e4 g4 c5") (voice 0 a))
(tables.packed_bytes picture ()" +
           Bytes(picture) + R"() :in aux)
(tables.packed_bytes few ()" +
           Bytes(few) + R"() :in aux)
(let ticks u16 0 :address 0x03C0)
(fn irq () void :interrupt
    (mb.service)
    (set ticks (+ ticks 1)))
(fn main () void
    (mb.init)
    )" + (guard ? "(auxmem.guard)" : "(cli)") +
           R"(
    (mb.play (addr_of (at hum 0)))
    (unpack.aux_to_card (as (ptr u8) 0xD000) (addr_of (at picture 0)) auxcard.BANK2)
    (unpack.aux_to_card (as (ptr u8) 0xD000) (addr_of (at few 0)) auxcard.BANK1)
    (mb.off))
)";
}

struct Run {
    bool ok = false;
    bool bytes = false;
    uint64_t counted = 0;
    uint64_t due = 0;
    uint64_t latest = 0;
    uint64_t cycles = 0;
};

Run RunCase(TestResults& results, const std::vector<uint8_t>& picture,
            const std::vector<uint8_t>& few, bool guard) {
    Run run;
    MbBuild b;
    MbBuildText(results, "t.tsp", Program(picture, few, guard), b);
    if (!b.ok) {
        return run;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    a->lc.write_enable = true; // as a program's startup in the card leaves it
    tisple::TickLog log;
    tisple::StartTickLog(log, *a);
    uint64_t start = a->cpu.cycles;
    while (!a->stopped && a->cpu.cycles - start < 600 * kMbTick) {
        tisple::RunApple2Traced(*a, 1000, tisple::TickLogStep, &log);
    }
    run.ok = a->stop_reason == tisple::MachineStop::kReturned;
    // the picture at $D000-$EFFF of the aux card (bank 2's 4 KB, then the shared 8 KB's first
    // 4), the few-valued bytes in bank 1
    bool same = true;
    for (size_t i = 0; i < picture.size(); i++) {
        uint8_t got = i < 0x1000 ? a->lc.aux_bank2_d000[i] : a->lc.aux_e000[i - 0x1000];
        same = same && got == picture[i];
    }
    for (size_t i = 0; i < few.size(); i++) {
        same = same && a->lc.aux_bank1_d000[i] == few[i];
    }
    run.bytes = same;
    run.counted = a->cpu.mem[kTicks] | (a->cpu.mem[kTicks + 1] << 8);
    // the ticks due from the first to the program's end: a lost one leaves the count short
    if (!log.ticks.empty()) {
        run.due = (a->cpu.cycles - log.ticks.front().taken) / kMbTick + 1;
    }
    run.cycles = a->cpu.cycles - start;
    for (const tisple::TickEntry& e : log.ticks) {
        run.latest = e.late > run.latest ? e.late : run.latest;
    }
    return run;
}

} // namespace

void RunUnpackCardTests(TestResults& results) {
    std::vector<uint8_t> picture = DhgrSample(5, 0);
    TestRandom r;
    std::vector<uint8_t> few(kBank1Bytes);
    for (uint8_t& b : few) {
        b = static_cast<uint8_t>(r.Below(4) * 0x55);
    }
    Run on = RunCase(results, picture, few, true);
    CHECK(results, on.ok && on.bytes);
    // every tick taken, none waiting a whole period
    CHECK(results, on.counted == on.due && on.counted > 20);
    CHECK(results, on.latest < kMbTick);
    Run off = RunCase(results, picture, few, false);
    CHECK(results, off.ok && off.bytes);
    CHECK(results, off.latest > 4 * kMbTick && off.counted + 4 < off.cycles / kMbTick);
    std::printf(
        "unpack into the aux card under the Mockingboard: with the guard all %llu ticks "
        "taken, the latest %llu cycles late, %llu cycles in all; without it %llu in %llu "
        "periods, one %llu late, %llu cycles\n",
        static_cast<unsigned long long>(on.counted), static_cast<unsigned long long>(on.latest),
        static_cast<unsigned long long>(on.cycles), static_cast<unsigned long long>(off.counted),
        static_cast<unsigned long long>(off.cycles / kMbTick),
        static_cast<unsigned long long>(off.latest), static_cast<unsigned long long>(off.cycles));
}

} // namespace tisple_tests
