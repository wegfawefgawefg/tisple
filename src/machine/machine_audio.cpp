#include "machine/machine_audio.hpp"

#include "machine/mockingboard.hpp"

namespace tisple {

void StartMachineAudio(MachineAudio& m, const Apple2& a) {
    m.speaker.next_toggle = a.speaker.toggle_count;
    m.speaker.samples_done = SamplesForCycles(Cycles(a));
    StartMockingboardAudio(m.chips, a);
    StartDriveSound(m.drives, a);
}

void RenderMachineAudio(MachineAudio& m, const Apple2& a, uint64_t cycle, bool drive_sound,
                        int channels, std::vector<int16_t>& out) {
    // the middle first, in mono: the speaker, then the drives into its samples
    size_t first = out.size();
    RenderSpeaker(m.speaker, a, cycle, out);
    if (drive_sound) {
        MixDriveSound(m.drives, a, cycle, out, first);
    }
    if (channels == 2) {
        SpreadToStereo(out, first);
    }
    if (HasMockingboard(a)) {
        MixMockingboard(m.chips, a, cycle, out, first, channels, m.stereo_width);
    }
}

void SpreadToStereo(std::vector<int16_t>& out, size_t first) {
    size_t count = out.size() - first;
    out.resize(first + count * 2);
    // from the end back, so each sample is read before its place is written
    for (size_t i = count; i-- > 0;) {
        int16_t s = out[first + i];
        out[first + i * 2] = s;
        out[first + i * 2 + 1] = s;
    }
}

} // namespace tisple
