// lib/mockingboard on the machine with the emulated card: detection with and without it, in
// other slots and over a noisy floating bus; examples/mockingboard_demo.tsp's player stepping
// from the timer interrupt (each voice's notes, read back from the sound chips' register
// writes, a tick apart as the patterns say), an effect from a key taking a voice and giving
// it back, the end turning the card off; with no card, the effect on the speaker. Then what a
// tick costs, in the demo and with every register changing every tick.

#include "machine/mockingboard.hpp"
#include "machine/mouse.hpp"
#include "test_backend.hpp"
#include "test_machine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

constexpr uint64_t kTick = 17030; // the lib's default rate
constexpr int kPulse = 8;         // the demo's patterns' length, in ticks

struct Build {
    tisple::Program program;
    tisple::Compiled code;
    bool ok = false;
};

void BuildText(TestResults& results, const std::string& path, const std::string& text, Build& b) {
    tisple::LoadOptions load;
    if (!text.empty()) {
        load.files.push_back(tisple::VirtualFile{path, text});
    }
    std::vector<tisple::Diagnostic> errors;
    b.ok = tisple::BuildProgram(path, load, b.program, errors) &&
           tisple::CompileProgram(b.program, tisple::BuildOptions(), b.code, errors);
    CHECK(results, b.ok);
    if (!b.ok) {
        std::fprintf(stderr, "%s", tisple::FormatDiagnostics(b.program.sources, errors).c_str());
    }
}

uint16_t VarAddress(const Build& b, const char* name) {
    int32_t v = tisple::FindVar(b.program, name);
    return v < 0 ? 0 : b.code.layout.vars[static_cast<size_t>(v)].address;
}

// loaded, the card in slot (0 for none), the IRQ vector at $FFFE in the card's ram straight to
// the program's entry, as with the card's ram in: no rom needed
std::unique_ptr<tisple::Apple2> Start(const Build& b, int slot) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    std::string error;
    for (const tisple::MemorySegment& s : b.code.segments) {
        tisple::LoadSegment(*a, s, error);
    }
    tisple::LoadProgram(*a, b.code.image.data(), b.code.image.size(), b.code.origin, error);
    if (slot != 0) {
        tisple::InsertMockingboard(*a, slot);
    }
    a->lc.read_ram = true;
    a->lc.e000[0x1FFE] = static_cast<uint8_t>(b.code.irq_direct & 0xFF);
    a->lc.e000[0x1FFF] = static_cast<uint8_t>(b.code.irq_direct >> 8);
    return a;
}

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
    Build b;
    BuildText(results, "t.tsp", kDetect, b);
    if (!b.ok) {
        return;
    }
    uint16_t seen = VarAddress(b, "t.seen");
    for (int slot : {0, 4, 2, 7}) {
        std::unique_ptr<tisple::Apple2> a = Start(b, slot);
        tisple::RunApple2(*a, 2000000);
        CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
        CHECK(results, a->cpu.mem[seen] == slot);
        // off: no interrupt left asserted or coming
        CHECK(results, a->cpu.irq_level_at == UINT64_MAX);
    }
    // the mouse card in 4 and the Mockingboard in 5
    std::unique_ptr<tisple::Apple2> a = Start(b, 5);
    tisple::InsertMouseCard(*a, tisple::kMouseSlot, nullptr);
    tisple::RunApple2(*a, 2000000);
    CHECK(results, a->cpu.mem[seen] == 5);
    // no card, a hi-res screen of noise on the floating bus
    std::unique_ptr<tisple::Apple2> noisy = Start(b, 0);
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
    Build b;
    BuildText(results, "t.tsp", kRateStop, b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = Start(b, 5);
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

// the cycles each interrupt takes, from the cpu's 7-cycle entry to the end of its RTI
struct TickCost {
    uint16_t entry = 0;
    uint64_t began = 0;
    bool inside = false;
    uint64_t count = 0;
    uint64_t total = 0;
    uint64_t most = 0;
    std::vector<uint64_t> each;
};

void MeasureTick(void* ctx, const tisple::Machine& m) {
    TickCost& t = *static_cast<TickCost*>(ctx);
    uint16_t pc = tisple::GetRegs(m).pc;
    if (pc == t.entry && !t.inside) {
        t.inside = true;
        t.began = m.cycles - 7;
        return;
    }
    if (t.inside && m.mem[pc] == 0x40) {
        uint64_t took = m.cycles + 6 - t.began;
        t.inside = false;
        t.count++;
        t.total += took;
        t.most = took > t.most ? took : t.most;
        t.each.push_back(took);
    }
}

// a chip's registers at each tick: the writes replayed, looked at after each interrupt
struct Voices {
    std::vector<uint16_t> period[6]; // a tick each
    std::vector<uint8_t> volume[6];
    std::vector<uint8_t> mixer[2];
};

const int kVoiceTone[6] = {0, 2, 4, 0, 2, 4};

Voices ReadVoices(const tisple::Apple2& a, uint64_t first_tick, int ticks) {
    Voices out;
    uint8_t regs[2][16] = {};
    const tisple::MockingboardState& mb = a.slots.mockingboard;
    uint64_t n = 0;
    for (int t = 0; t < ticks; t++) {
        uint64_t until = first_tick + static_cast<uint64_t>(t) * kTick + kTick / 2;
        while (n < mb.log_count && tisple::MockingboardWrite(a, n).cycle < until) {
            const tisple::PsgWrite& w = tisple::MockingboardWrite(a, n);
            if (w.reg < 16) {
                regs[w.chip][w.reg] = w.value;
            }
            n++;
        }
        for (int v = 0; v < 6; v++) {
            const uint8_t* r = regs[v / 3];
            out.period[v].push_back(
                static_cast<uint16_t>(r[kVoiceTone[v]] | (r[kVoiceTone[v] + 1] << 8)));
            out.volume[v].push_back(r[8 + v % 3]);
        }
        out.mixer[0].push_back(regs[0][7]);
        out.mixer[1].push_back(regs[1][7]);
    }
    return out;
}

// what the lib's NOTE_PERIOD holds for a note name's number (C1 = 0)
uint16_t Period(int note) {
    double f = 440.0 * std::pow(2.0, (note - 45) / 12.0);
    return static_cast<uint16_t>(std::lround(1020484.0 / (16.0 * f)));
}

int Note(char letter, bool sharp, int octave) {
    const int semis[7] = {9, 11, 0, 2, 4, 5, 7}; // a b c d e f g
    return (octave - 1) * 12 + semis[letter - 'a'] + (sharp ? 1 : 0);
}

// the first tick: the first interrupt's cycle, from the first write after the reset
uint64_t FirstTick(const tisple::Apple2& a) {
    const tisple::MockingboardState& mb = a.slots.mockingboard;
    for (uint64_t n = 0; n < mb.log_count; n++) {
        const tisple::PsgWrite& w = tisple::MockingboardWrite(a, n);
        if (w.chip == 0 && w.reg == 2 && w.value != 0) {
            return w.cycle - 2000; // inside the first tick, before its writes
        }
    }
    return 0;
}

void DemoTests(TestResults& results, TickCost& cost) {
    Build b;
    BuildText(results, "examples/mockingboard_demo.tsp", "", b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = Start(b, 4);
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
    for (const char* p = arp; *p != 0; p += 3) {
        arp_periods.push_back(Period(Note(p[0], false, p[1] - '0')));
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
    Build b;
    BuildText(results, "examples/mockingboard_demo.tsp", "", b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = Start(b, 0);
    tisple::ScheduleKey(*a, 20 * tisple::kCyclesPerFrame, '2');
    tisple::ScheduleKey(*a, 80 * tisple::kCyclesPerFrame, 0x1B);
    tisple::RunApple2(*a, 200 * tisple::kCyclesPerFrame);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, a->speaker.toggle_count > 100 && a->cpu.irqs_taken == 0);
    // coin: b5 for 4 ticks, e6 for 12: about 16 / 60 s of clicks at those pitches
    uint64_t want = (4 * kTick) / (8 * Period(Note('b', false, 5))) +
                    (12 * kTick) / (8 * Period(Note('e', false, 6)));
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
        (mb.effect (addr_of (at blip 0)) (% i 6) 1)
        (wait_cycles 8000))
    (mb.off))
)";

void BusyTests(TestResults& results, TickCost& cost) {
    Build b;
    BuildText(results, "t.tsp", kBusy, b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = Start(b, 4);
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
