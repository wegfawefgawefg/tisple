#pragma once

// the Mockingboard's two AY-3-8910s as 16-bit mono samples, mixed into the speaker's. the
// machine only logs each register write with its cycle; this replays the log through two
// AySynths at the chip clock / 8 (about 128 kHz) and box-filters down to kAudioRate. both
// chips go to the one channel (the card is stereo: the first AY left, the second right),
// with a high-pass like the speaker's to take off the chips' DC.

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
    double last_in = 0;
    double last_out = 0;
    uint64_t dropped = 0; // writes lost because the log wrapped before we read them
};

// start rendering at the machine's present cycle, from the chips' registers as they are
void StartMockingboardAudio(MockingboardRenderer& r, const Apple2& a);

// adds the chips' sound up to cycle into out[first ...], the samples RenderSpeaker has just
// appended for the same cycles (any it did not, it appends)
void MixMockingboard(MockingboardRenderer& r, const Apple2& a, uint64_t cycle,
                     std::vector<int16_t>& out, size_t first);

} // namespace tisple
