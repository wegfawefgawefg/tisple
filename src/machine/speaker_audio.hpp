#pragma once

// speaker toggles to 16-bit mono samples, and WAV files

#include "machine/apple2.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

constexpr int kAudioRate = 44100;

// consumer-side state: not part of the machine, so snapshots stay pure
struct SpeakerRenderer {
    uint64_t next_toggle = 0;  // index into the machine's toggle log
    uint64_t samples_done = 0; // sample n covers cycles [n, n + 1) * kCpuHz / kAudioRate
    bool high = false;         // cone position
    double last_in = 0;
    double last_out = 0;
    uint64_t dropped = 0; // toggles lost because the ring wrapped before we read them
};

// appends the samples that end at or before cycle
void RenderSpeaker(SpeakerRenderer& r, const Apple2& a, uint64_t cycle, std::vector<int16_t>& out);

uint64_t SamplesForCycles(uint64_t cycles);

bool WriteWav(const std::string& path, const std::vector<int16_t>& samples, std::string& error);

} // namespace tisple
