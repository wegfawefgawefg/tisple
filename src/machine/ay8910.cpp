#include "machine/ay8910.hpp"

#include <array>
#include <cmath>
#include <cstring>

namespace tisple {

namespace {

constexpr uint8_t kRegisterMasks[16] = {0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
                                        0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF};

constexpr uint8_t kBusReset = 0x00; // /RESET low: the other bits do not matter
constexpr uint8_t kBusRead = 0x05;
constexpr uint8_t kBusWrite = 0x06;
constexpr uint8_t kBusLatch = 0x07;

// about 3 dB a step: each one 1/sqrt(2) of the next, 0 silent
std::array<double, 16> BuildLevels() {
    std::array<double, 16> levels = {};
    for (int v = 1; v < 16; v++) {
        levels[v] = std::pow(2.0, (v - 15) / 2.0);
    }
    return levels;
}

const std::array<double, 16> kLevels = BuildLevels();

uint16_t TonePeriod(const AySynth& s, int channel) {
    uint16_t period = static_cast<uint16_t>(s.regs[channel * 2] | (s.regs[channel * 2 + 1] << 8));
    return period == 0 ? 1 : period;
}

void StepNoise(AySynth& s) {
    uint8_t period = s.regs[6] == 0 ? 1 : s.regs[6];
    s.noise_count++;
    if (s.noise_count < period) {
        return;
    }
    s.noise_count = 0;
    uint32_t bit = (s.noise_shift ^ (s.noise_shift >> 3)) & 1;
    s.noise_shift = (s.noise_shift >> 1) | (bit << 16);
}

void SetEnvelopeLevel(AySynth& s) {
    s.envelope_level = static_cast<uint8_t>(s.envelope_up ? s.envelope_step : 15 - s.envelope_step);
}

// the end of a ramp: by the shape, hold, go round again, or turn round
void EndRamp(AySynth& s) {
    uint8_t shape = s.regs[13];
    bool cont = (shape & 0x08) != 0;
    bool alt = (shape & 0x02) != 0;
    bool hold = (shape & 0x01) != 0;
    if (!cont) {
        s.envelope_holding = true;
        s.envelope_level = 0;
        return;
    }
    if (hold) {
        s.envelope_holding = true;
        bool top = s.envelope_up != alt; // the ramp's last level, or its opposite with ALT
        s.envelope_level = top ? 15 : 0;
        return;
    }
    if (alt) {
        s.envelope_up = !s.envelope_up;
    }
    s.envelope_step = 0;
    SetEnvelopeLevel(s);
}

void StepEnvelope(AySynth& s) {
    uint16_t period = static_cast<uint16_t>(s.regs[11] | (s.regs[12] << 8));
    period = period == 0 ? 1 : period;
    s.envelope_count++;
    if (s.envelope_count < period) {
        return;
    }
    s.envelope_count = 0;
    if (s.envelope_holding) {
        return;
    }
    s.envelope_step++;
    if (s.envelope_step == 16) {
        EndRamp(s);
        return;
    }
    SetEnvelopeLevel(s);
}

void RestartEnvelope(AySynth& s) {
    s.envelope_count = 0;
    s.envelope_step = 0;
    s.envelope_up = (s.regs[13] & 0x04) != 0;
    s.envelope_holding = false;
    SetEnvelopeLevel(s);
}

} // namespace

uint8_t AyRegisterMask(int reg) {
    return kRegisterMasks[reg & 0x0F];
}

bool AyBus(AyChip& ay, uint8_t port_b, uint8_t port_a, uint8_t& reg, uint8_t& value) {
    uint8_t bus = static_cast<uint8_t>(port_b & 0x07);
    if ((bus & 0x04) == 0) {
        bus = kBusReset;
    }
    // the chip follows its lines: act when they or the data change
    bool same = bus == ay.bus && port_a == ay.data;
    ay.bus = bus;
    ay.data = port_a;
    if (same) {
        return false;
    }
    if (bus == kBusReset) {
        std::memset(ay.regs, 0, sizeof(ay.regs));
        ay.address = 0;
        ay.selected = true;
        reg = kPsgLogReset;
        value = 0;
        return true;
    }
    if (bus == kBusLatch) {
        ay.selected = (port_a & 0xF0) == 0;
        ay.address = static_cast<uint8_t>(port_a & 0x0F);
        return false;
    }
    if (bus != kBusWrite || !ay.selected) {
        return false;
    }
    reg = ay.address;
    value = static_cast<uint8_t>(port_a & kRegisterMasks[reg]);
    ay.regs[reg] = value;
    return true;
}

uint8_t AyDataOut(const AyChip& ay) {
    if (ay.bus != kBusRead || !ay.selected) {
        return 0xFF;
    }
    return ay.regs[ay.address];
}

void ResetAySynth(AySynth& s) {
    s = AySynth{};
    RestartEnvelope(s);
}

void AySynthWrite(AySynth& s, uint8_t reg, uint8_t value) {
    if (reg == kPsgLogReset) {
        ResetAySynth(s);
        return;
    }
    s.regs[reg & 0x0F] = value;
    if ((reg & 0x0F) == 13) {
        RestartEnvelope(s);
    }
}

double AySynthTick(AySynth& s) {
    for (int c = 0; c < 3; c++) {
        s.tone_count[c]++;
        if (s.tone_count[c] >= TonePeriod(s, c)) {
            s.tone_count[c] = 0;
            s.tone_high[c] = !s.tone_high[c];
        }
    }
    s.odd_tick = !s.odd_tick;
    if (s.odd_tick) {
        StepNoise(s);
        StepEnvelope(s);
    }
    bool noise_high = (s.noise_shift & 1) != 0;
    uint8_t mixer = s.regs[7];
    double sum = 0;
    for (int c = 0; c < 3; c++) {
        bool tone = s.tone_high[c] || (mixer & (1 << c)) != 0;
        bool noise = noise_high || (mixer & (8 << c)) != 0;
        if (!tone || !noise) {
            continue;
        }
        uint8_t volume = s.regs[8 + c];
        sum += kLevels[(volume & 0x10) != 0 ? s.envelope_level : (volume & 0x0F)];
    }
    return sum;
}

} // namespace tisple
