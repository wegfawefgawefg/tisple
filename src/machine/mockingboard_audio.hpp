#pragma once

// the Mockingboard's two AY-3-8910s as 16-bit samples, mixed into the speaker's. the
// machine only logs each register write with its cycle; this replays the log through two
// AySynths at the chip clock / 8 (about 128 kHz) and box-filters down to kAudioRate. the card
// is stereo, as this is by default: the first AY on the left, the second on the right. in
// mono both go to the one channel. each chip has a high-pass like the speaker's to take off
// its DC (the filter is linear, so the two summed are the one the mono mix had on their sum).

#include "machine/apple2.hpp"
#include "machine/ay8910.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tisple {

// consumer-side state, like SpeakerRenderer: not part of the machine
struct MockingboardRenderer {
    uint64_t next_write = 0;   // index into the card's write log
    uint64_t samples_done = 0; // as SpeakerRenderer's
    uint64_t ticks_done = 0;   // chip clock / 8 steps since cycle 0
    AySynth chips[2];
    double last_in[2] = {}; // each chip's high-pass
    double last_out[2] = {};
    uint64_t dropped = 0; // writes lost because the log wrapped before we read them
};

// start rendering at the machine's present cycle, from the chips' registers as they are
void StartMockingboardAudio(MockingboardRenderer& r, const Apple2& a);

// adds the chips' sound up to cycle into out[first ...], the frames the speaker and drives
// have just made for the same cycles (any they did not, it appends). channels 2: frames of
// left and right, the first chip on the left, the second on the right; 1: both in the one
void MixMockingboard(MockingboardRenderer& r, const Apple2& a, uint64_t cycle,
                     std::vector<int16_t>& out, size_t first, int channels);

} // namespace tisple
