#include "machine/speaker_audio.hpp"

#include <cstdio>

namespace tisple {

namespace {

constexpr double kCyclesPerSample = kCpuHz / kAudioRate;
// one-pole high-pass, about 35 Hz: the cone drifts back to rest when nothing clicks it
constexpr double kHighPass = 0.995;
constexpr double kVolume = 0.35 * 32767.0;

void PutLe16(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void PutLe32(std::vector<uint8_t>& out, uint32_t value) {
    PutLe16(out, value & 0xFFFF);
    PutLe16(out, value >> 16);
}

void PutTag(std::vector<uint8_t>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

} // namespace

uint64_t SamplesForCycles(uint64_t cycles) {
    return static_cast<uint64_t>(static_cast<double>(cycles) / kCyclesPerSample);
}

void RenderSpeaker(SpeakerRenderer& r, const Apple2& a, uint64_t cycle, std::vector<int16_t>& out) {
    uint64_t count = a.speaker.toggle_count;
    if (count - r.next_toggle > kSpeakerRingSize) {
        r.dropped += count - kSpeakerRingSize - r.next_toggle;
        r.next_toggle = count - kSpeakerRingSize;
    }
    uint64_t target = SamplesForCycles(cycle);
    while (r.samples_done < target) {
        double begin = static_cast<double>(r.samples_done) * kCyclesPerSample;
        double end = begin + kCyclesPerSample;
        // average the cone position over the sample: a box filter against aliasing
        double sum = 0;
        double at = begin;
        while (r.next_toggle < count) {
            double toggle = static_cast<double>(SpeakerToggleCycle(a, r.next_toggle));
            if (toggle >= end) {
                break;
            }
            if (toggle > at) {
                sum += (r.high ? 1.0 : -1.0) * (toggle - at);
                at = toggle;
            }
            r.high = !r.high;
            r.next_toggle++;
        }
        sum += (r.high ? 1.0 : -1.0) * (end - at);
        double in = sum / kCyclesPerSample;
        double filtered = in - r.last_in + kHighPass * r.last_out;
        r.last_in = in;
        r.last_out = filtered;
        double sample = filtered * kVolume;
        sample = sample > 32767.0 ? 32767.0 : (sample < -32768.0 ? -32768.0 : sample);
        out.push_back(static_cast<int16_t>(sample));
        r.samples_done++;
    }
}

bool WriteWav(const std::string& path, const std::vector<int16_t>& samples, std::string& error) {
    uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);
    std::vector<uint8_t> bytes;
    bytes.reserve(44 + data_bytes);
    PutTag(bytes, "RIFF");
    PutLe32(bytes, 36 + data_bytes);
    PutTag(bytes, "WAVE");
    PutTag(bytes, "fmt ");
    PutLe32(bytes, 16);
    PutLe16(bytes, 1); // pcm
    PutLe16(bytes, 1); // mono
    PutLe32(bytes, kAudioRate);
    PutLe32(bytes, kAudioRate * 2);
    PutLe16(bytes, 2);
    PutLe16(bytes, 16);
    PutTag(bytes, "data");
    PutLe32(bytes, data_bytes);
    for (int16_t s : samples) {
        PutLe16(bytes, static_cast<uint16_t>(s));
    }
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        error = "cannot write '" + path + "'";
        return false;
    }
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    ok = std::fclose(file) == 0 && ok;
    if (!ok) {
        error = "cannot write '" + path + "'";
    }
    return ok;
}

} // namespace tisple
