// the Mockingboard: its 6522s' timers, flags and IRQ level on their own; the IRQ line and
// the counter through the 6502 on the machine; the AY-3-8910s' registers through the 6522
// ports, read back, and the rendered sound's pitch; stereo: each chip on its own side, the
// speaker in both, and mono their sum

#include "machine/ay8910.hpp"
#include "machine/machine_audio.hpp"
#include "machine/mockingboard.hpp"
#include "machine/mockingboard_audio.hpp"
#include "machine/slots.hpp"
#include "machine/speaker_audio.hpp"
#include "machine/via6522.hpp"
#include "test_machine.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

using tisple::Via6522;
using tisple::ViaRead;
using tisple::ViaTimer1;
using tisple::ViaTimer2;
using tisple::ViaWrite;

constexpr int kSlot = 4;

uint8_t Ifr(Via6522& v, uint64_t cycle) {
    return ViaRead(v, 13, cycle, 0xFF, 0xFF);
}

// T1 loaded with 1000 at cycle 100, one-shot: N at w + 1, $FFFF and the flag at w + N + 2,
// then on down without another flag
void OneShotTests(TestResults& results) {
    Via6522 v;
    ViaWrite(v, 4, 0xE8, 99);
    ViaWrite(v, 5, 0x03, 100);
    CHECK(results, ViaTimer1(v, 101) == 1000);
    CHECK(results, ViaTimer1(v, 600) == 501);
    CHECK(results, ViaTimer1(v, 1101) == 0);
    CHECK(results, ViaTimer1(v, 1102) == 0xFFFF);
    CHECK(results, ViaTimer1(v, 1103) == 0xFFFE);
    CHECK(results, Ifr(v, 1101) == 0x00);
    CHECK(results, Ifr(v, 1102) == 0x40);                    // not enabled: bit 7 stays clear
    CHECK(results, ViaRead(v, 4, 1110, 0xFF, 0xFF) == 0xF7); // $FFFF - 8: and clears it
    CHECK(results, Ifr(v, 1111) == 0x00);
    CHECK(results, Ifr(v, 1102 + 70000) == 0x00); // through 0 again: no flag
    // the latches read back without touching the counter
    CHECK(results, ViaRead(v, 6, 1200, 0xFF, 0xFF) == 0xE8);
    CHECK(results, ViaRead(v, 7, 1200, 0xFF, 0xFF) == 0x03);
    // reloading starts it again
    ViaWrite(v, 5, 0x00, 2000); // latch $00E8
    CHECK(results, ViaTimer1(v, 2001) == 0xE8);
    CHECK(results, Ifr(v, 2000 + 0xE8 + 2) == 0x40);
}

// free-run: every N + 2 cycles, a flag at each $FFFF; a new latch counts from the next reload
void FreeRunTests(TestResults& results) {
    Via6522 v;
    ViaWrite(v, 11, 0x40, 0);
    ViaWrite(v, 4, 100, 1);
    ViaWrite(v, 5, 0, 2); // underflows at 104, 206, 308
    CHECK(results, ViaTimer1(v, 3) == 100);
    CHECK(results, ViaTimer1(v, 103) == 0);
    CHECK(results, ViaTimer1(v, 104) == 0xFFFF);
    CHECK(results, ViaTimer1(v, 105) == 100);
    CHECK(results, ViaTimer1(v, 206) == 0xFFFF);
    CHECK(results, Ifr(v, 104) == 0x40);
    ViaRead(v, 4, 150, 0xFF, 0xFF);
    CHECK(results, Ifr(v, 205) == 0x00);
    CHECK(results, Ifr(v, 206) == 0x40);
    ViaWrite(v, 6, 50, 210); // the low latch: the count to 308 goes on, then 50s
    CHECK(results, ViaTimer1(v, 307) == 0);
    CHECK(results, ViaTimer1(v, 308) == 0xFFFF);
    CHECK(results, ViaTimer1(v, 309) == 50);
    CHECK(results, ViaTimer1(v, 360) == 0xFFFF);
    // enabled: the IRQ output is low while the flag is set, and IFR shows bit 7
    ViaWrite(v, 14, 0xC0, 400);
    CHECK(results, ViaRead(v, 14, 401, 0xFF, 0xFF) == 0xC0);
    CHECK(results, Ifr(v, 401) == 0xC0);
    CHECK(results, tisple::ViaIrqCycle(v) == 0);
    ViaWrite(v, 13, 0x40, 402); // writing a one clears the flag
    CHECK(results, tisple::ViaIrqCycle(v) == 412);
    CHECK(results, Ifr(v, 411) == 0x00);
    CHECK(results, Ifr(v, 412) == 0xC0);
    ViaWrite(v, 14, 0x40, 413); // disabled
    CHECK(results, ViaRead(v, 14, 414, 0xFF, 0xFF) == 0x80);
    CHECK(results, tisple::ViaIrqCycle(v) == tisple::kViaNever);
    CHECK(results, Ifr(v, 415) == 0x40);
    // back to one-shot: the next underflow comes, the reloads stop counting
    ViaWrite(v, 11, 0x00, 420);
    CHECK(results, ViaTimer1(v, 463) == 0);
    CHECK(results, ViaTimer1(v, 465) == 0xFFFE);
}

// T2: one-shot, its flag once
void Timer2Tests(TestResults& results) {
    Via6522 v;
    ViaWrite(v, 14, 0xA0, 0);
    ViaWrite(v, 8, 10, 1);
    ViaWrite(v, 9, 0, 2); // N = 10: $FFFF at 14
    CHECK(results, ViaTimer2(v, 3) == 10);
    CHECK(results, tisple::ViaIrqCycle(v) == 14);
    CHECK(results, Ifr(v, 13) == 0x00);
    CHECK(results, Ifr(v, 14) == 0xA0);
    CHECK(results, ViaRead(v, 8, 20, 0xFF, 0xFF) == 0xF9);
    CHECK(results, tisple::ViaIrqCycle(v) == tisple::kViaNever);
    CHECK(results, Ifr(v, 70000) == 0x00);
}

// ports: outputs give the register, inputs what is outside
void PortTests(TestResults& results) {
    Via6522 v;
    ViaWrite(v, 3, 0x0F, 0);
    ViaWrite(v, 1, 0xA5, 1);
    CHECK(results, tisple::ViaPortA(v) == 0xF5);
    CHECK(results, ViaRead(v, 1, 2, 0x30, 0xFF) == 0x35);
    tisple::ResetVia(v, 3);
    CHECK(results, tisple::ViaPortA(v) == 0xFF && tisple::ViaPortB(v) == 0xFF);
}

// the card on the machine, at a chosen cycle
void Access(tisple::Apple2& a, uint64_t cycle, int offset, bool is_write, uint8_t value,
            uint8_t* got = nullptr) {
    a.cpu.cycles = cycle + 1;
    bool drives = false;
    uint8_t read = tisple::CardRomAccess(a, kSlot, offset, is_write, value, drives);
    if (got != nullptr) {
        *got = read;
    }
}

// the six accesses of one register write, for the AY on chip's 6522
uint64_t WritePsg(tisple::Apple2& a, uint64_t cycle, int chip, uint8_t reg, uint8_t value) {
    int base = chip * 0x80;
    Access(a, cycle, base + 1, true, reg);
    Access(a, cycle + 4, base + 0, true, 0x07);
    Access(a, cycle + 8, base + 0, true, 0x04);
    Access(a, cycle + 12, base + 1, true, value);
    Access(a, cycle + 16, base + 0, true, 0x06);
    Access(a, cycle + 20, base + 0, true, 0x04);
    return cycle + 24;
}

uint8_t ReadPsg(tisple::Apple2& a, uint64_t cycle, int chip, uint8_t reg) {
    int base = chip * 0x80;
    uint8_t got = 0;
    Access(a, cycle, base + 1, true, reg);
    Access(a, cycle + 4, base + 0, true, 0x07);
    Access(a, cycle + 8, base + 0, true, 0x04);
    Access(a, cycle + 12, base + 3, true, 0x00); // port A in
    Access(a, cycle + 16, base + 0, true, 0x05);
    Access(a, cycle + 20, base + 1, false, 0, &got);
    Access(a, cycle + 24, base + 0, true, 0x04);
    Access(a, cycle + 28, base + 3, true, 0xFF);
    return got;
}

uint64_t InitPsgs(tisple::Apple2& a, uint64_t cycle) {
    for (int chip = 0; chip < 2; chip++) {
        int base = chip * 0x80;
        Access(a, cycle, base + 2, true, 0xFF);
        Access(a, cycle + 4, base + 3, true, 0xFF);
        Access(a, cycle + 8, base + 0, true, 0x00); // reset
        Access(a, cycle + 12, base + 0, true, 0x04);
        cycle += 16;
    }
    return cycle;
}

// rising zero crossings a second, over the second half of the samples
double Pitch(const std::vector<int16_t>& samples) {
    size_t from = samples.size() / 2;
    int crossings = 0;
    for (size_t i = from + 1; i < samples.size(); i++) {
        crossings += samples[i - 1] < 0 && samples[i] >= 0 ? 1 : 0;
    }
    double seconds = static_cast<double>(samples.size() - from) / tisple::kAudioRate;
    return crossings / seconds;
}

double Loudness(const std::vector<int16_t>& samples) {
    double sum = 0;
    size_t from = samples.size() / 2;
    for (size_t i = from; i < samples.size(); i++) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    return std::sqrt(sum / static_cast<double>(samples.size() - from));
}

// a second of one tone: chip, register 0-1 or 2-3 or 4-5 of channel, the period, the mixer
std::vector<int16_t> RenderTone(int chip, int channel, uint16_t period, uint8_t mixer,
                                uint8_t volume) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    tisple::InsertMockingboard(*a, kSlot);
    tisple::MockingboardRenderer r;
    tisple::StartMockingboardAudio(r, *a);
    uint64_t at = InitPsgs(*a, 100);
    at = WritePsg(*a, at, chip, static_cast<uint8_t>(channel * 2), period & 0xFF);
    at = WritePsg(*a, at, chip, static_cast<uint8_t>(channel * 2 + 1), period >> 8);
    at = WritePsg(*a, at, chip, 7, mixer);
    WritePsg(*a, at, chip, static_cast<uint8_t>(8 + channel), volume);
    std::vector<int16_t> samples;
    tisple::MixMockingboard(r, *a, static_cast<uint64_t>(tisple::kCpuHz), samples, 0, 1);
    return samples;
}

void PsgTests(TestResults& results) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    tisple::InsertMockingboard(*a, kSlot);
    uint64_t at = InitPsgs(*a, 100);
    at = WritePsg(*a, at, 0, 1, 0xFF); // coarse tone keeps 4 bits
    at = WritePsg(*a, at, 1, 8, 0x1F);
    at = WritePsg(*a, at, 1, 0x13, 0x55); // latch $13 deselects: ignored
    CHECK(results, a->slots.mockingboard.ay[0].regs[1] == 0x0F);
    CHECK(results, a->slots.mockingboard.ay[1].regs[8] == 0x1F);
    CHECK(results, a->slots.mockingboard.ay[1].regs[3] == 0x00);
    CHECK(results, ReadPsg(*a, at, 0, 1) == 0x0F);
    CHECK(results, ReadPsg(*a, at + 40, 1, 8) == 0x1F);
    // the reset line clears them
    tisple::ResetMockingboard(*a);
    CHECK(results, a->slots.mockingboard.ay[0].regs[1] == 0x00);

    // pitch: clock / (16 * period)
    double want = tisple::kCpuHz / (16.0 * 256);
    double got = Pitch(RenderTone(0, 0, 256, 0x3E, 15));
    double want_second = tisple::kCpuHz / (16.0 * 100);
    double second = Pitch(RenderTone(1, 2, 100, 0x3B, 12));
    bool pitched = std::fabs(got - want) < 2.0 && std::fabs(second - want_second) < 3.0;
    CHECK(results, pitched);
    if (!pitched) {
        std::fprintf(stderr, "mockingboard pitch %.1f (want %.1f), %.1f (want %.1f)\n", got, want,
                     second, want_second);
    }
    // the mixer's tone bit off, or volume 0: silence
    double loud = Loudness(RenderTone(0, 0, 256, 0x3E, 15));
    double off = Loudness(RenderTone(0, 0, 256, 0x3F, 15));
    double quiet = Loudness(RenderTone(0, 0, 256, 0x3E, 0));
    CHECK(results, loud > 1000.0 && off < 10.0 && quiet < 10.0);
}

// what StereoTests plays: chip 0's tone A, chip 1's tone C, the speaker's square, each when on
struct StereoParts {
    bool chip0 = false;
    bool chip1 = false;
    bool speaker = false;
};

// a second of the parts through RenderMachineAudio in channels (no drives' sound)
std::vector<int16_t> RenderParts(StereoParts parts, int channels) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    tisple::InsertMockingboard(*a, kSlot);
    tisple::MachineAudio sound;
    tisple::StartMachineAudio(sound, *a);
    uint64_t at = InitPsgs(*a, 100);
    // the same writes at the same cycles every time, so the tones keep their phase: a part
    // that is off has volume 0
    at = WritePsg(*a, at, 0, 0, 0x00); // period 256
    at = WritePsg(*a, at, 0, 1, 0x01);
    at = WritePsg(*a, at, 0, 7, 0x3E);
    at = WritePsg(*a, at, 0, 8, parts.chip0 ? 15 : 0);
    at = WritePsg(*a, at, 1, 4, 100); // period 100
    at = WritePsg(*a, at, 1, 5, 0);
    at = WritePsg(*a, at, 1, 7, 0x3B);
    at = WritePsg(*a, at, 1, 10, parts.chip1 ? 12 : 0);
    uint64_t end = static_cast<uint64_t>(tisple::kCpuHz);
    if (parts.speaker) {
        // a click every 940 cycles: a square of about 544 Hz
        for (uint64_t cycle = 1000; cycle < end; cycle += 940) {
            a->speaker.toggles[a->speaker.toggle_count % tisple::kSpeakerRingSize] = cycle;
            a->speaker.toggle_count++;
        }
    }
    std::vector<int16_t> out;
    // in slices, as the window and --wav render it
    for (uint64_t cycle = at + 17030; cycle < end; cycle += 17030) {
        tisple::RenderMachineAudio(sound, *a, cycle, false, channels, out);
    }
    tisple::RenderMachineAudio(sound, *a, end, false, channels, out);
    return out;
}

// channel c (0 left, 1 right) of stereo frames
std::vector<int16_t> Side(const std::vector<int16_t>& frames, int c) {
    std::vector<int16_t> out;
    for (size_t i = static_cast<size_t>(c); i < frames.size(); i += 2) {
        out.push_back(frames[i]);
    }
    return out;
}

// the largest difference between two runs of samples, or a big number when their sizes differ
int MostApart(const std::vector<int16_t>& x, const std::vector<int16_t>& y) {
    if (x.size() != y.size()) {
        return 1 << 20;
    }
    int most = 0;
    for (size_t i = 0; i < x.size(); i++) {
        int d = x[i] - y[i];
        most = d < 0 ? (-d > most ? -d : most) : (d > most ? d : most);
    }
    return most;
}

void StereoTests(TestResults& results) {
    std::vector<int16_t> left_only = RenderParts({true, false, false}, 2);
    std::vector<int16_t> right_only = RenderParts({false, true, false}, 2);
    std::vector<int16_t> speaker = RenderParts({false, false, true}, 2);
    std::vector<int16_t> mono_speaker = RenderParts({false, false, true}, 1);
    size_t frames = tisple::SamplesForCycles(static_cast<uint64_t>(tisple::kCpuHz));
    CHECK(results, left_only.size() == frames * 2 && mono_speaker.size() == frames);

    // a write to chip 0 sounds only on the left, at its pitch; one to chip 1 only on the right
    double want_left = tisple::kCpuHz / (16.0 * 256);
    double want_right = tisple::kCpuHz / (16.0 * 100);
    CHECK(results, Loudness(Side(left_only, 0)) > 1000.0 && Loudness(Side(left_only, 1)) < 1.0);
    CHECK(results, Loudness(Side(right_only, 1)) > 1000.0 && Loudness(Side(right_only, 0)) < 1.0);
    CHECK(results, std::fabs(Pitch(Side(left_only, 0)) - want_left) < 2.0);
    CHECK(results, std::fabs(Pitch(Side(right_only, 1)) - want_right) < 3.0);
    // each at the level it has in mono
    std::vector<int16_t> mono_left = RenderParts({true, false, false}, 1);
    std::vector<int16_t> mono_right = RenderParts({false, true, false}, 1);
    CHECK(results, MostApart(Side(left_only, 0), mono_left) <= 1);
    CHECK(results, MostApart(Side(right_only, 1), mono_right) <= 1);

    // the speaker in the middle: the same in both, and as it is in mono
    CHECK(results, Loudness(mono_speaker) > 1000.0);
    CHECK(results, Side(speaker, 0) == Side(speaker, 1) && Side(speaker, 0) == mono_speaker);

    // all three: each side the speaker and its chip, and mono the sum of all of them, as the
    // one channel was before stereo
    std::vector<int16_t> all = RenderParts({true, true, true}, 2);
    std::vector<int16_t> mono_all = RenderParts({true, true, true}, 1);
    // (each render has the speaker's cone at rest at the start, settling: a chip's part is its
    // render less that of a silent machine)
    std::vector<int16_t> silent = RenderParts({false, false, false}, 1);
    std::vector<int16_t> left_sum, right_sum, sum;
    for (size_t i = 0; i < frames; i++) {
        int left = mono_left[i] - silent[i];
        int right = mono_right[i] - silent[i];
        left_sum.push_back(static_cast<int16_t>(mono_speaker[i] + left));
        right_sum.push_back(static_cast<int16_t>(mono_speaker[i] + right));
        sum.push_back(static_cast<int16_t>(mono_speaker[i] + left + right));
    }
    int left_apart = MostApart(Side(all, 0), left_sum);
    int right_apart = MostApart(Side(all, 1), right_sum);
    int mono_apart = MostApart(mono_all, sum);
    // each part truncates to a whole sample on its own: a step or two apart
    bool summed = left_apart <= 2 && right_apart <= 2 && mono_apart <= 3;
    CHECK(results, summed);
    if (!summed) {
        std::fprintf(stderr, "stereo: left %d, right %d, mono %d steps from the parts' sums\n",
                     left_apart, right_apart, mono_apart);
    }

    // a stereo frame's speaker part is mono's: the spread keeps every sample, in order
    std::vector<int16_t> spread = mono_speaker;
    tisple::SpreadToStereo(spread, 0);
    CHECK(results, spread == speaker);
}

// a 6502 program on the machine with the timer's IRQ through the card's ram vector at $FFFE.
// its handler counts in $10 and answers only every other time, so the line stays low and the
// cpu comes straight back in: a level, not a request
std::vector<uint8_t> IrqProgram() {
    return {
        0x78,                                           // 6000 SEI
        0xA9, 0x40,                                     // LDA #$40
        0x8D, 0x0B, 0xC4,                               // STA ACR: T1 free-run
        0xA9, 0xE8,                                     // LDA #$E8
        0x8D, 0x04, 0xC4,                               // STA T1C-L
        0xA9, 0x03,                                     // LDA #$03
        0x8D, 0x05, 0xC4,                               // STA T1C-H: 1000, started (cycle 15)
        0xAD, 0x04, 0xC4,                               // LDA T1C-L: read 4 cycles later, 997
        0x85, 0x11,                                     // STA $11
        0xA9, 0xC0,                                     // LDA #$C0
        0x8D, 0x0E, 0xC4,                               // STA IER
        0x58,                                           // CLI
        0xA5, 0x10,                                     // 601B LDA $10
        0xC9, 0x14,                                     // CMP #20
        0x90, 0xFA,                                     // BCC $601B
        0x78,                                           // SEI
        0xA9, 0x7F,                                     // LDA #$7F
        0x8D, 0x0E, 0xC4,                               // STA IER
        0x60,                                           // RTS
        0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, // to $6030
        0x48,                                           // 6030 PHA
        0xE6, 0x10,                                     // INC $10
        0xA5, 0x10,                                     // LDA $10
        0x29, 0x01,                                     // AND #$01
        0xF0, 0x03,                                     // BEQ +3
        0xAD, 0x04, 0xC4,                               // LDA T1C-L: answers it
        0x68,                                           // PLA
        0x40,                                           // RTI
    };
}

void IrqTests(TestResults& results) {
    std::vector<uint8_t> program = IrqProgram();
    CHECK(results, program.size() == 0x3E);
    std::unique_ptr<tisple::Apple2> a = MachineWithBytes(program);
    tisple::InsertMockingboard(*a, kSlot);
    a->lc.read_ram = true;
    a->lc.e000[0x1FFE] = 0x30;
    a->lc.e000[0x1FFF] = 0x60;
    tisple::RunOutcome r = RunToStop(*a);
    CHECK(results, r.reason == tisple::MachineStop::kReturned);
    CHECK(results, a->cpu.mem[0x11] == 0xE5); // 997
    // entries 2k and 2k + 1 come at the same underflow: 21 entries by the 11th
    uint64_t taken = a->cpu.irqs_taken;
    CHECK(results, a->cpu.mem[0x10] == taken && (taken == 20 || taken == 21));
    CHECK(results, r.cycles > 10 * 1002 && r.cycles < 11 * 1002 + 200);
    if (r.cycles <= 10 * 1002 || r.cycles >= 11 * 1002 + 200) {
        std::fprintf(stderr, "mockingboard irq: %llu interrupts in %llu cycles\n",
                     static_cast<unsigned long long>(taken),
                     static_cast<unsigned long long>(r.cycles));
    }
    CHECK(results, a->cpu.irq_level_at == UINT64_MAX); // disabled at the end
    // no card: the reads float, and nothing interrupts
    std::unique_ptr<tisple::Apple2> empty = MachineWithBytes({0xAD, 0x04, 0xC4, 0x60});
    RunToStop(*empty);
    CHECK(results, empty->cpu.irqs_taken == 0 && empty->cpu.irq_level_at == UINT64_MAX);
}

} // namespace

void RunMachineMockingboardTests(TestResults& results) {
    OneShotTests(results);
    FreeRunTests(results);
    Timer2Tests(results);
    PortTests(results);
    PsgTests(results);
    StereoTests(results);
    IrqTests(results);
}

} // namespace tisple_tests
