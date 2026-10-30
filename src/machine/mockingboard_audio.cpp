#include "machine/mockingboard_audio.hpp"

#include "machine/mockingboard.hpp"
#include "machine/speaker_audio.hpp"

namespace tisple {

namespace {

constexpr double kCyclesPerSample = kCpuHz / kAudioRate;
constexpr uint64_t kCyclesPerTick = 8; // the chip's tone counters step at its clock / 8
// one channel at full volume: a quarter of full scale. six at full volume in phase would
// clip, which music at the usual levels (8-13: a tenth to a half of full) never does
constexpr double kChannelVolume = 0.25 * 32767.0;
constexpr double kHighPass = 0.995; // about 35 Hz, as the speaker's

int16_t Clamp(double sample) {
    if (sample > 32767.0) {
        return 32767;
    }
    if (sample < -32768.0) {
        return -32768;
    }
    return static_cast<int16_t>(sample);
}

// the writes made in cycles before end
void ApplyWrites(MockingboardRenderer& r, const Apple2& a, uint64_t end) {
    uint64_t count = a.slots.mockingboard.log_count;
    while (r.next_write < count) {
        const PsgWrite& w = MockingboardWrite(a, r.next_write);
        if (w.cycle >= end) {
            return;
        }
        AySynthWrite(r.chips[w.chip & 1], w.reg, w.value);
        r.next_write++;
    }
}

} // namespace

void StartMockingboardAudio(MockingboardRenderer& r, const Apple2& a) {
    const MockingboardState& mb = a.slots.mockingboard;
    uint64_t now = Cycles(a);
    r = MockingboardRenderer{};
    r.samples_done = SamplesForCycles(now);
    r.ticks_done = now / kCyclesPerTick;
    r.next_write = mb.log_count;
    for (int chip = 0; chip < 2; chip++) {
        ResetAySynth(r.chips[chip]);
        for (int reg = 0; reg < 16; reg++) {
            AySynthWrite(r.chips[chip], static_cast<uint8_t>(reg), mb.ay[chip].regs[reg]);
        }
    }
}

void MixMockingboard(MockingboardRenderer& r, const Apple2& a, uint64_t cycle,
                     std::vector<int16_t>& out, size_t first) {
    uint64_t count = a.slots.mockingboard.log_count;
    if (count - r.next_write > kPsgLogSize) {
        r.dropped += count - kPsgLogSize - r.next_write;
        r.next_write = count - kPsgLogSize;
    }
    uint64_t target = SamplesForCycles(cycle);
    size_t at = first;
    while (r.samples_done < target) {
        double end = static_cast<double>(r.samples_done + 1) * kCyclesPerSample;
        // the average of the chips' ticks that end inside this sample
        double sum = 0;
        int ticks = 0;
        while (static_cast<double>((r.ticks_done + 1) * kCyclesPerTick) <= end) {
            ApplyWrites(r, a, (r.ticks_done + 1) * kCyclesPerTick);
            sum += AySynthTick(r.chips[0]) + AySynthTick(r.chips[1]);
            ticks++;
            r.ticks_done++;
        }
        double in = ticks > 0 ? sum / ticks : r.last_in;
        double filtered = in - r.last_in + kHighPass * r.last_out;
        r.last_in = in;
        r.last_out = filtered;
        double sample = filtered * kChannelVolume;
        if (at < out.size()) {
            out[at] = Clamp(out[at] + sample);
        } else {
            out.push_back(Clamp(sample));
        }
        at++;
        r.samples_done++;
    }
}

} // namespace tisple
