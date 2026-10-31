#include "machine/drive_audio.hpp"

#include "machine/disk2.hpp"
#include "machine/speaker_audio.hpp"

#include <cmath>

namespace tisple {

namespace {

constexpr double kCyclesPerSample = kCpuHz / kAudioRate;
constexpr double kPi = 3.14159265358979323846;
constexpr double kFullScale = 32767.0;

// levels, in full scale. the speaker's square is 0.35 and a Mockingboard channel 0.25
constexpr double kImpactVolume = 0.03; // a tick's resonators; a knock is kKnockGain of it
constexpr double kKnockGain = 2.2;
constexpr double kTickBurstVolume = 0.02; // the click of noise on a tick
constexpr double kKnockBurstVolume = 0.04;
constexpr double kMotorVolume = 0.03;  // the whir at speed
constexpr double kThumpVolume = 0.018; // the spindle's thump once a turn

constexpr double kSpinUpSeconds = 0.12;   // the spin level's time constant at motor on
constexpr double kSpinDownSeconds = 0.4;  // and from full to still once the drive stops
constexpr double kTurnsPerSecond = 5.0;   // 300 rpm
constexpr double kHumHz = 47.0;           // the motor's shaft at speed: the hum's fundamental
constexpr double kBounceSeconds = 0.0032; // a knock's carriage comes back off the stop
constexpr double kBounceGain = 0.4;
// the head's pace (see the .hpp)
constexpr double kStepSettleSeconds = 0.0025; // it strikes again no sooner
constexpr double kKnockSettleSeconds = 0.036; // nor after a knock
constexpr double kCatchUpSeconds = 0.0197;    // behind, a step each phase of Apple's rom
constexpr double kMostBehindSeconds = 2.0;    // impacts later than this are dropped
constexpr double kQuiet = 1e-6;               // below this everything has died away: output silence

// the resonators: the carriage's and the chassis's modes, how long each rings, and how hard
// a tick and a knock strike it
struct Mode {
    double hz;
    double decay_ms;
    double tick;
    double knock;
};

constexpr Mode kModes[kDriveModes] = {
    {85, 45, 0.04, 0.85},  // the chassis: a knock's thud
    {190, 26, 0.15, 0.75}, //
    {440, 15, 0.45, 0.65}, // the carriage
    {1050, 5, 0.75, 0.45}, //
    {2200, 3, 0.8, 0.3},   // the tick's metal
    {3900, 1.6, 0.55, 0.2}, {6300, 1.0, 0.3, 0.1},
};

struct Bandpass {
    double b0 = 0;
    double a1 = 0;
    double a2 = 0;
};

struct Coefficients {
    double mode_a1[kDriveModes] = {};
    double mode_a2[kDriveModes] = {};
    double mode_in[kDriveModes] = {};
    double thump_a1 = 0;
    double thump_a2 = 0;
    double thump_in = 0;
    Bandpass swish_low;
    Bandpass swish_high;
    double spin_up = 0;
    double spin_down = 0;
    double tick_burst_decay = 0;
    double knock_burst_decay = 0;
    uint64_t bounce_samples = 0;
    uint64_t step_settle = 0; // samples from an impact to the head's next
    uint64_t knock_settle = 0;
    uint64_t catch_up = 0;
    uint64_t most_behind = 0;
};

// a two-pole resonator: an impulse of 1 rings at about 1 and dies by e every decay_ms
void SetResonator(double hz, double decay_ms, double& a1, double& a2, double& in) {
    double w = 2 * kPi * hz / kAudioRate;
    double r = std::exp(-1000.0 / (decay_ms * kAudioRate));
    a1 = 2 * r * std::cos(w);
    a2 = r * r;
    in = std::sin(w);
}

// the bilinear band-pass with 0 dB at its centre (b1 is 0, b2 is -b0)
Bandpass MakeBandpass(double hz, double q) {
    double w = 2 * kPi * hz / kAudioRate;
    double alpha = std::sin(w) / (2 * q);
    double a0 = 1 + alpha;
    return Bandpass{alpha / a0, -2 * std::cos(w) / a0, (1 - alpha) / a0};
}

double Decay(double seconds) {
    return std::exp(-1.0 / (seconds * kAudioRate));
}

Coefficients MakeCoefficients() {
    Coefficients c;
    for (int m = 0; m < kDriveModes; m++) {
        SetResonator(kModes[m].hz, kModes[m].decay_ms, c.mode_a1[m], c.mode_a2[m], c.mode_in[m]);
    }
    SetResonator(80, 45, c.thump_a1, c.thump_a2, c.thump_in);
    c.swish_low = MakeBandpass(850, 0.7);
    c.swish_high = MakeBandpass(3200, 1.2);
    c.spin_up = 1 - Decay(kSpinUpSeconds);
    c.spin_down = 1.0 / (kSpinDownSeconds * kAudioRate);
    c.tick_burst_decay = Decay(0.0007);
    c.knock_burst_decay = Decay(0.0022);
    c.bounce_samples = static_cast<uint64_t>(kBounceSeconds * kAudioRate);
    c.step_settle = static_cast<uint64_t>(kStepSettleSeconds * kAudioRate);
    c.knock_settle = static_cast<uint64_t>(kKnockSettleSeconds * kAudioRate);
    c.catch_up = static_cast<uint64_t>(kCatchUpSeconds * kAudioRate);
    c.most_behind = static_cast<uint64_t>(kMostBehindSeconds * kAudioRate);
    return c;
}

const Coefficients& Coeffs() {
    static const Coefficients c = MakeCoefficients();
    return c;
}

double RunBandpass(const Bandpass& f, DriveBandpass& s, double x) {
    double y = f.b0 * x + s.z1;
    s.z1 = -f.a1 * y + s.z2;
    s.z2 = -f.b0 * x - f.a2 * y;
    return y;
}

// uniform in [0, 1)
double Random(DriveSoundRenderer& r) {
    uint32_t x = r.noise;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r.noise = x;
    return static_cast<double>(x) / 4294967296.0;
}

double Noise(DriveSoundRenderer& r) {
    return Random(r) * 2 - 1;
}

// the strikes this sample: how hard each resonator is hit
struct Strikes {
    double modes[kDriveModes] = {};
    bool any = false;
};

// each mode a little more or less each time, so no two impacts ring quite alike
void Strike(DriveSoundRenderer& r, Strikes& s, const DriveImpact& impact) {
    for (int m = 0; m < kDriveModes; m++) {
        double share = impact.knock ? kModes[m].knock : kModes[m].tick;
        s.modes[m] += impact.gain * share * (0.7 + 0.6 * Random(r));
    }
    s.any = true;
    if (impact.knock) {
        r.knock_burst += impact.gain * kKnockBurstVolume;
    } else {
        r.tick_burst += impact.gain * kTickBurstVolume;
    }
}

// a step or a knock waits for the head, in order
void QueueImpact(DriveSoundRenderer& r, bool knock) {
    const Coefficients& c = Coeffs();
    uint64_t when = r.head_free > r.samples_done ? r.head_free : r.samples_done;
    if (r.queue_count == kDriveQueue || when - r.samples_done > c.most_behind) {
        r.dropped++;
        return;
    }
    DriveImpact& impact = r.queue[(r.queue_head + r.queue_count) % kDriveQueue];
    r.queue_count++;
    impact.sample = when;
    impact.gain = (0.8 + 0.4 * Random(r)) * (knock ? kKnockGain : 1); // no two quite alike
    impact.knock = knock;
    bool late = when > r.samples_done;
    uint64_t settle = late ? c.catch_up : c.step_settle;
    r.head_free = when + (knock ? c.knock_settle : settle);
    r.late += late ? 1 : 0;
}

void ApplyEvent(DriveSoundRenderer& r, const DriveEvent& e) {
    if (e.kind == DriveEventKind::kSpin) {
        r.motor_on = e.motor_on;
        r.spin_until = e.spin_until;
        r.drive = e.drive;
        return;
    }
    QueueImpact(r, e.kind == DriveEventKind::kStop);
}

// the impacts due this sample, and the bounces they set going
void StrikeDue(DriveSoundRenderer& r, Strikes& s) {
    const Coefficients& c = Coeffs();
    int kept = 0;
    for (int i = 0; i < r.bounce_count; i++) {
        if (r.bounces[i].sample <= r.samples_done) {
            Strike(r, s, r.bounces[i]);
        } else {
            r.bounces[kept++] = r.bounces[i];
        }
    }
    r.bounce_count = kept;
    while (r.queue_count > 0 && r.queue[r.queue_head].sample <= r.samples_done) {
        const DriveImpact& impact = r.queue[r.queue_head];
        Strike(r, s, impact);
        r.impacts++;
        if (impact.knock && r.bounce_count < kDriveBounces) {
            DriveImpact& b = r.bounces[r.bounce_count++];
            b.sample = r.samples_done + c.bounce_samples;
            b.gain = impact.gain * kBounceGain;
            b.knock = true;
        }
        r.queue_head = (r.queue_head + 1) % kDriveQueue;
        r.queue_count--;
    }
}

// the events made in cycles before end
void ApplyEvents(DriveSoundRenderer& r, const Apple2& a, double end) {
    uint64_t count = a.slots.disk2.log_count;
    while (r.next_event < count) {
        const DriveEvent& e = DriveLogEvent(a, r.next_event);
        if (static_cast<double>(e.cycle) >= end) {
            return;
        }
        ApplyEvent(r, e);
        r.next_event++;
    }
}

bool Spinning(const DriveSoundRenderer& r, int drive, double cycle) {
    return drive == r.drive && (r.motor_on || cycle < static_cast<double>(r.spin_until));
}

// one drive's motor, spindle and disk for one sample, and its thump into thump_in
double RunMotor(DriveSoundRenderer& r, DriveMotorVoice& v, bool spinning, double& thump_in) {
    const Coefficients& c = Coeffs();
    if (spinning) {
        v.level += (1 - v.level) * c.spin_up;
    } else {
        v.level -= c.spin_down; // the disk coasts down
        if (v.level <= 0) {
            v.level = 0;
            return 0;
        }
    }
    v.flutter += (Noise(r) - v.flutter) * 0.0004;
    double speed = v.level * (1 + 0.004 * std::sin(2 * kPi * v.turn) + 0.15 * v.flutter);
    v.turn += kTurnsPerSecond * speed / kAudioRate;
    if (v.turn >= 1) {
        v.turn -= 1;
        thump_in += v.level * v.level;
    }
    v.hum += kHumHz * speed / kAudioRate;
    v.hum -= std::floor(v.hum);
    // the disk rubs its jacket hardest once a turn
    double rub = 0.5 + 0.5 * std::cos(2 * kPi * v.turn);
    rub = rub * rub * rub * rub;
    double n = Noise(r);
    double swish = RunBandpass(c.swish_low, v.swish_low, n) +
                   0.45 * RunBandpass(c.swish_high, v.swish_high, n);
    double angle = 2 * kPi * v.hum;
    double hum = std::sin(angle) + 0.5 * std::sin(2 * angle) + 0.3 * std::sin(3 * angle) +
                 0.12 * std::sin(8 * angle);
    double whir = swish * (0.6 + 0.6 * rub) + 0.18 * hum * v.level;
    return whir * v.level * kMotorVolume;
}

bool Quiet(const DriveSoundRenderer& r) {
    if (r.queue_count > 0 || r.bounce_count > 0 || r.motors[0].level > 0 || r.motors[1].level > 0 ||
        r.tick_burst > kQuiet || r.knock_burst > kQuiet || std::fabs(r.thump[0]) > kQuiet ||
        std::fabs(r.thump[1]) > kQuiet) {
        return false;
    }
    for (int m = 0; m < kDriveModes; m++) {
        if (std::fabs(r.modes[m][0]) > kQuiet || std::fabs(r.modes[m][1]) > kQuiet) {
            return false;
        }
    }
    return true;
}

// one sample of everything, with this sample's strikes
double RunSample(DriveSoundRenderer& r, const Strikes& s, double cycle) {
    const Coefficients& c = Coeffs();
    double impacts = 0;
    for (int m = 0; m < kDriveModes; m++) {
        double x = s.modes[m] * c.mode_in[m];
        double y = c.mode_a1[m] * r.modes[m][0] - c.mode_a2[m] * r.modes[m][1] + x;
        r.modes[m][1] = r.modes[m][0];
        r.modes[m][0] = y;
        impacts += y;
    }
    // the clicks: noise with its lows taken off, under the resonators' ring
    double n = Noise(r);
    double click = (n - r.last_noise) * 0.5 * (r.tick_burst + r.knock_burst);
    r.last_noise = n;
    r.tick_burst *= c.tick_burst_decay;
    r.knock_burst *= c.knock_burst_decay;
    double thump_in = 0;
    double motor = 0;
    for (int d = 0; d < 2; d++) {
        motor += RunMotor(r, r.motors[d], Spinning(r, d, cycle), thump_in);
    }
    double thump = c.thump_a1 * r.thump[0] - c.thump_a2 * r.thump[1] + thump_in * c.thump_in;
    r.thump[1] = r.thump[0];
    r.thump[0] = thump;
    return impacts * kImpactVolume + click + motor + thump * kThumpVolume;
}

int16_t Clamp(double sample) {
    if (sample > 32767.0) {
        return 32767;
    }
    if (sample < -32768.0) {
        return -32768;
    }
    return static_cast<int16_t>(sample);
}

} // namespace

void StartDriveSound(DriveSoundRenderer& r, const Apple2& a) {
    const Disk2State& card = a.slots.disk2;
    uint64_t now = Cycles(a);
    r = DriveSoundRenderer{};
    r.samples_done = SamplesForCycles(now);
    r.next_event = card.log_count;
    r.motor_on = card.motor_on;
    r.spin_until = card.spin_until;
    r.drive = card.drive;
    if (DriveSpinning(a, now)) {
        r.motors[card.drive].level = 1; // already at speed
    }
}

void MixDriveSound(DriveSoundRenderer& r, const Apple2& a, uint64_t cycle,
                   std::vector<int16_t>& out, size_t first) {
    uint64_t count = a.slots.disk2.log_count;
    if (count - r.next_event > kDriveLogSize) {
        r.dropped += count - kDriveLogSize - r.next_event;
        r.next_event = count - kDriveLogSize;
    }
    uint64_t target = SamplesForCycles(cycle);
    size_t at = first;
    while (r.samples_done < target) {
        double begin = static_cast<double>(r.samples_done) * kCyclesPerSample;
        double end = begin + kCyclesPerSample;
        Strikes s;
        ApplyEvents(r, a, end);
        StrikeDue(r, s);
        bool silent = !s.any && !Spinning(r, 0, begin) && !Spinning(r, 1, begin) && Quiet(r);
        double sample = silent ? 0 : RunSample(r, s, begin) * kFullScale;
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
