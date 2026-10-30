// lib/mockingboard on the machine with the emulated card: detection with and without it, in
// other slots and over a noisy floating bus; examples/mockingboard_demo.tsp's player stepping
// from the timer interrupt (each voice's notes, read back from the sound chips' register
// writes, a tick apart as the patterns say), an effect from a key taking a voice and giving
// it back, the end turning the card off; with no card, the effect on the speaker. Then what a
// tick costs, in the demo and with every register changing every tick.

#include "machine/mockingboard.hpp"
#include "machine/mouse.hpp"
#include "test_mockingboard.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

constexpr int kPulse = 8; // the demo's patterns' length, in ticks

const char* kDetect = R"(
(import mockingboard mb)
(let seen u8 0 :address 0x0310) ; outside zero page, which the end puts back
(fn irq () void :interrupt (mb.service))
(fn main () void
    (mb.init)
    (set seen (if mb.found mb.slot 0))
    (mb.off))
)";

void DetectTests(TestResults& results) {
    MbBuild b;
    MbBuildText(results, "t.tsp", kDetect, b);
    if (!b.ok) {
        return;
    }
    uint16_t seen = MbVarAddress(b, "t.seen");
    for (int slot : {0, 4, 2, 7}) {
        std::unique_ptr<tisple::Apple2> a = MbStart(b, slot);
        tisple::RunApple2(*a, 2000000);
        CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
        CHECK(results, a->cpu.mem[seen] == slot);
        // off: no interrupt left asserted or coming
        CHECK(results, a->cpu.irq_level_at == UINT64_MAX);
    }
    // the mouse card in 4 and the Mockingboard in 5
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 5);
    tisple::InsertMouseCard(*a, tisple::kMouseSlot, nullptr);
    tisple::RunApple2(*a, 2000000);
    CHECK(results, a->cpu.mem[seen] == 5);
    // no card, a hi-res screen of noise on the floating bus
    std::unique_ptr<tisple::Apple2> noisy = MbStart(b, 0);
    noisy->sw.text = false;
    noisy->sw.hires = true;
    uint32_t x = 12345;
    for (int i = 0x2000; i < 0x4000; i++) {
        x = x * 1103515245u + 12345u;
        noisy->cpu.mem[i] = static_cast<uint8_t>(x >> 16);
    }
    tisple::RunApple2(*noisy, 2000000);
    CHECK(results, noisy->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, noisy->cpu.mem[seen] == 0);
}

// set_rate: twice as many ticks. stop: the music's voices quiet, the tick going on
const char* kRateStop = R"(
(import mockingboard mb)
(import mockingboard_song tune)
(tune.song hum (pattern a "l4 v12 c4 e4 g4") (voice 0 a) (voice 4 a))
(fn irq () void :interrupt (mb.service))
(fn main () void
    (mb.init)
    (mb.set_rate 8515)
    (mb.play (addr_of (at hum 0)))
    (for i u8 0 20 (wait_cycles 50000))
    (mb.stop)
    (while true))
)";

void RateStopTests(TestResults& results) {
    MbBuild b;
    MbBuildText(results, "t.tsp", kRateStop, b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 5);
    tisple::RunApple2(*a, 2000000);
    // about 1.1 M cycles to the stop at 120 a second, then on to 2 M
    uint64_t taken = a->cpu.irqs_taken;
    CHECK(results, taken > 2000000 / 8515 - 6 && taken < 2000000 / 8515 + 2);
    const uint8_t* first = a->slots.mockingboard.ay[0].regs;
    const uint8_t* second = a->slots.mockingboard.ay[1].regs;
    CHECK(results, first[8] == 0 && second[8] == 0 && second[9] == 0);
    CHECK(results, first[0] != 0); // it did play
    CHECK(results, a->slots.mockingboard.slot == 5);
}

void DemoTests(TestResults& results, TickCost& cost) {
    MbBuild b;
    MbBuildText(results, "examples/mockingboard_demo.tsp", "", b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    // 1 at frame 300: zap on voice 5. ESC at 600 ends it
    tisple::ScheduleKey(*a, 300 * tisple::kCyclesPerFrame, '1');
    tisple::ScheduleKey(*a, 600 * tisple::kCyclesPerFrame, 0x1B);
    cost.entry = b.code.irq_direct;
    tisple::RunApple2Traced(*a, 700 * tisple::kCyclesPerFrame, MeasureTick, &cost);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, a->cpu.irqs_taken > 590 && a->cpu.irqs_taken < 605);
    CHECK(results, a->cpu.irq_level_at == UINT64_MAX);
    uint64_t first = FirstTick(*a);
    Voices v = ReadVoices(*a, first, 590);
    // voice 1: the arpeggio, a note every pulse
    const char* arp = "d4 a4 c5 e5 a4 d5 c5 a4 g4 a4 e5 c5";
    std::vector<uint16_t> arp_periods;
    // two characters a note, a space between (none after the last)
    for (const char* p = arp;; p += 3) {
        arp_periods.push_back(Period(Note(p[0], false, p[1] - '0')));
        if (p[2] == 0) {
            break;
        }
    }
    bool arp_ok = true;
    bool slip_ok = true;
    for (int t = 0; t < 576; t++) {
        int pulse = t / kPulse;
        arp_ok = arp_ok && v.period[1][t] == arp_periods[pulse % 12];
        // voice 3: the same with a rest, 13 pulses, so a pulse later each time round
        if (pulse % 13 == 12) {
            slip_ok = slip_ok && v.volume[3][t] == 0;
        } else {
            slip_ok = slip_ok && v.period[3][t] == arp_periods[pulse % 13];
        }
    }
    CHECK(results, arp_ok && slip_ok);
    // the arpeggio's decay: v11 k1, one less a tick from the note's start
    CHECK(results, v.volume[1][0] == 11 && v.volume[1][1] == 10 && v.volume[1][7] == 4 &&
                       v.volume[1][kPulse] == 11);
    // the shaker on voice 5 is noise; the zap at frame 300 puts tones there, a tick each, then
    // gives the voice back
    CHECK(results, (v.mixer[1][0] & 0x24) == 0x04);
    int zap_at = -1;
    for (int t = 280; t < 330 && zap_at < 0; t++) {
        if (v.period[5][t] == Period(Note('c', false, 7))) {
            zap_at = t;
        }
    }
    CHECK(results, zap_at > 0);
    if (zap_at > 0) {
        const char* zap = "c7 g6 d6 a5 e5 b4 f#4 c#4 g#3";
        int t = zap_at;
        bool zap_ok = true;
        for (const char* p = zap; *p != 0; t++) {
            bool sharp = p[1] == '#';
            int octave = p[sharp ? 2 : 1] - '0';
            zap_ok = zap_ok && v.period[5][t] == Period(Note(p[0], sharp, octave)) &&
                     v.volume[5][t] == 15 && (v.mixer[1][t] & 0x24) == 0x20;
            p += sharp ? 3 : 2;
            p += *p == ' ' ? 1 : 0;
        }
        CHECK(results, zap_ok);
        CHECK(results, (v.mixer[1][t + 2] & 0x24) == 0x04); // the shaker's noise again
    }
    // the end: every volume 0
    const uint8_t* end0 = a->slots.mockingboard.ay[0].regs;
    const uint8_t* end1 = a->slots.mockingboard.ay[1].regs;
    CHECK(results, end0[8] == 0 && end0[9] == 0 && end0[10] == 0 && end1[8] == 0 && end1[9] == 0 &&
                       end1[10] == 0);
}

// no card: the key's effect on the speaker, main waiting for it
void SpeakerTests(TestResults& results) {
    MbBuild b;
    MbBuildText(results, "examples/mockingboard_demo.tsp", "", b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 0);
    tisple::ScheduleKey(*a, 20 * tisple::kCyclesPerFrame, '2');
    tisple::ScheduleKey(*a, 80 * tisple::kCyclesPerFrame, 0x1B);
    tisple::RunApple2(*a, 200 * tisple::kCyclesPerFrame);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, a->speaker.toggle_count > 100 && a->cpu.irqs_taken == 0);
    // coin: b5 for 4 ticks, e6 for 12: about 16 / 60 s of clicks at those pitches
    uint64_t want = (4 * kMbTick) / (8 * Period(Note('b', false, 5))) +
                    (12 * kMbTick) / (8 * Period(Note('e', false, 6)));
    CHECK(results, a->speaker.toggle_count > want * 8 / 10 && a->speaker.toggle_count < want);
}

// every voice changes everything every tick: notes, volumes, mixer, noise and envelope; and
// an effect starts over and over
const char* kBusy = R"(
(import mockingboard mb)
(import mockingboard_song tune)
(tune.song busy
    (pattern a "l1 k1 v15 m3 n3 h14:300 c4 v14 m1 n7 h10:200 d5 v13 m3 n9 h12:250 e6 v12 m2 n1 h8:90 f3")
    (voice 0 a) (voice 1 a) (voice 2 a) (voice 3 a) (voice 4 a) (voice 5 a))
(tune.effect blip "l1 v15 m3 n5 c5 v14 m1 n6 d6")
(fn irq () void :interrupt (mb.service))
(fn main () void
    (mb.init)
    (mb.play (addr_of (at busy 0)))
    (for i u8 0 60
        (mb.effect (addr_of (at blip 0)) (% i 6) (+ i 1)) ; each cuts off the one before
        (wait_cycles 8000))
    (mb.off))
)";

void BusyTests(TestResults& results, TickCost& cost) {
    MbBuild b;
    MbBuildText(results, "t.tsp", kBusy, b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    cost.entry = b.code.irq_direct;
    // the handler takes most of the cpu: main's 60 waits of 8000 cycles take a while
    tisple::RunApple2Traced(*a, 600 * tisple::kCyclesPerFrame, MeasureTick, &cost);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, cost.count > 30);
}

} // namespace

void RunMockingboardLibTests(TestResults& results) {
    DetectTests(results);
    RateStopTests(results);
    TickCost demo;
    DemoTests(results, demo);
    SpeakerTests(results);
    TickCost busy;
    BusyTests(results, busy);
    if (demo.count == 0 || busy.count == 0) {
        return;
    }
    std::vector<uint64_t> sorted = demo.each;
    std::sort(sorted.begin(), sorted.end());
    std::printf("mockingboard: a tick in the demo %llu cycles at most (the song's first), %llu "
                "the next most, %llu on average, %llu the median; %llu at most with every "
                "register changing every tick\n",
                static_cast<unsigned long long>(demo.most),
                static_cast<unsigned long long>(sorted[sorted.size() - 2]),
                static_cast<unsigned long long>(demo.total / demo.count),
                static_cast<unsigned long long>(sorted[sorted.size() / 2]),
                static_cast<unsigned long long>(busy.most));
}

} // namespace tisple_tests
