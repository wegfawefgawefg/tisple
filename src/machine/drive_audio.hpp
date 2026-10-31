#pragma once

// the disk ii drives' sound as 16-bit mono samples, mixed into the speaker's. made from the
// card's event log (DriveEvent: steps, pulls against the stop, the motor) by a small synth,
// timed to the events' cycles as the speaker is to its toggles; nothing is recorded.
// - a head step: a bank of damped resonators (the carriage and the chassis) struck lightly,
//   mostly in its upper modes, with a short click of noise: a tick. a fast seek is a run of
//   ticks, a buzz.
// - a pull against the stop: the same bank struck hard and low, with a longer burst of noise,
//   and the carriage bounces back off the stop once a few milliseconds later: a knock. the
//   boot's recalibration is a run of these, the drive's machine-gun grind.
// - the motor, while the drive spins: band-passed noise (the belt, the bearings, the disk in
//   its jacket) that swells once a turn of the spindle, 300 rpm, a soft low thump each turn,
//   the motor's hum, and a little flutter in the speed. it ramps up at motor on and down when
//   the drive stops (the card's own off delay later).
// the head is a mechanism: it strikes at most once every 2.5 ms (DOS 3.3 seeks a little
// slower), and after a knock it bounces and settles for 36 ms (Apple's rom pulls it against
// the stop again 39 ms later). impacts that come sooner wait their turn, in order, and a head
// that has fallen behind catches up at the pace of Apple's rom, a step every 19.7 ms; more
// than two seconds behind, they are dropped. so steps at any real drive's pace sound at their
// cycle, and our own boot rom, which waits for nothing (80 phases in 2 ms), grinds for a second
// and a half like Apple's (the steps that follow it then sound late).
// both drives sound the same, into the one channel; all of it sits below the speaker and the
// Mockingboard.

#include "machine/apple2.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tisple {

constexpr int kDriveModes = 7;   // the resonators an impact strikes
constexpr int kDriveQueue = 128; // impacts waiting for the head
constexpr int kDriveBounces = 8; // knocks' bounces still to come

// a step or a knock, or a knock's bounce, still to strike
struct DriveImpact {
    uint64_t sample = 0; // when it strikes
    double gain = 0;
    bool knock = false;
};

struct DriveBandpass {
    double z1 = 0;
    double z2 = 0;
};

// one drive's motor, spindle and disk
struct DriveMotorVoice {
    double level = 0;   // 0 stopped, 1 at speed
    double turn = 0;    // the spindle's angle, in turns
    double hum = 0;     // the motor's, in turns
    double flutter = 0; // low-passed noise on the speed
    DriveBandpass swish_low;
    DriveBandpass swish_high;
};

// consumer-side state, like SpeakerRenderer: not part of the machine
struct DriveSoundRenderer {
    uint64_t next_event = 0;   // index into the card's event log
    uint64_t samples_done = 0; // as SpeakerRenderer's
    // the card as the last event applied left it
    bool motor_on = false;
    uint64_t spin_until = 0;
    uint8_t drive = 0;
    double modes[kDriveModes][2] = {}; // each resonator's last two outputs
    double tick_burst = 0;             // the noise bursts' envelopes
    double knock_burst = 0;
    double last_noise = 0;
    double thump[2] = {};           // the spindle's thump resonator
    DriveImpact queue[kDriveQueue]; // in order: impacts waiting for the head
    uint32_t queue_head = 0;
    uint32_t queue_count = 0;
    uint64_t head_free = 0; // the first sample the head can strike again
    DriveImpact bounces[kDriveBounces];
    int bounce_count = 0;
    uint64_t impacts = 0; // struck, for the tests and the status line
    uint64_t late = 0;    // of them, struck later than their events: the head was busy
    DriveMotorVoice motors[2];
    uint32_t noise = 0x6A09E667; // xorshift32: the same sound every run
    uint64_t dropped = 0;        // events lost: the log wrapped, or the head was a second behind
};

// start rendering at the machine's present cycle, with the drives as they are
void StartDriveSound(DriveSoundRenderer& r, const Apple2& a);

// adds the drives' sound up to cycle into out[first ...], the samples RenderSpeaker has just
// appended for the same cycles (any it did not, it appends)
void MixDriveSound(DriveSoundRenderer& r, const Apple2& a, uint64_t cycle,
                   std::vector<int16_t>& out, size_t first);

} // namespace tisple
