#pragma once

// all the machine's sound as 16-bit frames, for --wav, the window and the phone player: the
// speaker and the disk drives in the middle (the same in both channels), the Mockingboard's
// first AY to the left and its second to the right, stereo_width apart (1: hard left and
// right, as the card's two outputs are). in mono all of it goes to the one channel, as it did
// before stereo.

#include "machine/apple2.hpp"
#include "machine/drive_audio.hpp"
#include "machine/mockingboard_audio.hpp"
#include "machine/speaker_audio.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tisple {

// consumer-side state, like the renderers it holds: not part of the machine
struct MachineAudio {
    SpeakerRenderer speaker;
    MockingboardRenderer chips;
    DriveSoundRenderer drives;
    double stereo_width = kDefaultStereoWidth; // the chips' spread in stereo, 0-1: StereoGains
};

// start rendering at the machine's present cycle: what came before is skipped. the speaker's
// cone and filter carry on where they were
void StartMachineAudio(MachineAudio& m, const Apple2& a);

// appends the frames that end at or before cycle: channels 1 (mono) or 2 (left, right). the
// drives' sound only when drive_sound, the Mockingboard's when a has one
void RenderMachineAudio(MachineAudio& m, const Apple2& a, uint64_t cycle, bool drive_sound,
                        int channels, std::vector<int16_t>& out);

// the mono samples out[first ...] as stereo frames, each the same in both channels
void SpreadToStereo(std::vector<int16_t>& out, size_t first);

} // namespace tisple
