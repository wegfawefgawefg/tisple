// the disk ii's sound: the card's event log (steps, pulls against the stop, the motor) and
// the renderer that turns it into samples

#include "machine/disk2.hpp"
#include "machine/drive_audio.hpp"
#include "machine/speaker_audio.hpp"
#include "test_disk.hpp"

#include <cmath>

namespace tisple_tests {

namespace {

constexpr double kCyclesPerSample = tisple::kCpuHz / tisple::kAudioRate;

// one card access at a given cycle, as the cpu would make it
void Access(tisple::Apple2& a, int reg, uint64_t cycle) {
    a.cpu.cycles = cycle + 1; // the bus counts a cycle before performing it
    bool drives = false;
    tisple::Disk2IoAccess(a, reg, false, 0, drives);
}

// the head one half-track up from h: the next phase on, this one off
void StepUp(tisple::Apple2& a, int h, uint64_t& t) {
    Access(a, ((h + 1) & 3) * 2 + 1, t += 1000);
    Access(a, (h & 3) * 2, t += 1000);
}

const tisple::DriveEvent& Event(const tisple::Apple2& a, uint64_t n) {
    return tisple::DriveLogEvent(a, n);
}

bool IsEvent(const tisple::DriveEvent& e, tisple::DriveEventKind kind, uint64_t cycle,
             int half_track) {
    return e.kind == kind && e.cycle == cycle && e.half_track == half_track;
}

// motor on and off, steps, a pull against the stop, each logged at its cycle; nothing for a
// switch that changes nothing
void TestCardEvents(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> disk = NoiseDisk(31);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*disk);
    const tisple::Disk2State& card = a->slots.disk2;
    uint64_t base = card.log_count;
    Access(*a, 0x7, 100); // phase 3 with the motor off: no power, no pull
    Access(*a, 0x6, 110);
    CHECK(results, card.log_count == base);
    Access(*a, 0x9, 200); // motor on
    CHECK(results, card.log_count == base + 1);
    const tisple::DriveEvent& on = Event(*a, base);
    CHECK(results,
          IsEvent(on, tisple::DriveEventKind::kSpin, 200, 0) && on.motor_on && on.drive == 0);
    Access(*a, 0x9, 300); // on again: nothing changes
    CHECK(results, card.log_count == base + 1);

    // phase 3 at track 0 pulls the head outward against the stop: a knock
    Access(*a, 0x7, 1000);
    CHECK(results, card.log_count == base + 2);
    CHECK(results, IsEvent(Event(*a, base + 1), tisple::DriveEventKind::kStop, 1000, 0));
    Access(*a, 0x7, 1100); // the same switch again: the magnet is already on
    Access(*a, 0x6, 1200); // off: no pull
    Access(*a, 0x5, 1300); // phase 2, opposite the head: no pull
    Access(*a, 0x4, 1400);
    CHECK(results, card.log_count == base + 2);

    // two half-tracks up and one back: a step each, with the head where it went
    uint64_t t = 2000;
    StepUp(*a, 0, t);
    StepUp(*a, 1, t);
    Access(*a, 0x3, t += 1000); // phase 1: back to half-track 1
    Access(*a, 0x4, t += 1000); // phase 2 off: nothing pulls
    CHECK(results, card.log_count == base + 5);
    CHECK(results, IsEvent(Event(*a, base + 2), tisple::DriveEventKind::kStep, 3000, 1));
    CHECK(results, IsEvent(Event(*a, base + 3), tisple::DriveEventKind::kStep, 5000, 2));
    CHECK(results, IsEvent(Event(*a, base + 4), tisple::DriveEventKind::kStep, 7000, 1));

    // drive 2, then the motor off: it spins on for the card's delay
    Access(*a, 0xB, 9000);
    Access(*a, 0x8, 9500);
    Access(*a, 0x8, 9600); // off again: nothing
    CHECK(results, card.log_count == base + 7);
    const tisple::DriveEvent& select = Event(*a, base + 5);
    CHECK(results, select.kind == tisple::DriveEventKind::kSpin && select.cycle == 9000 &&
                       select.drive == 1 && select.motor_on);
    const tisple::DriveEvent& off = Event(*a, base + 6);
    CHECK(results, off.kind == tisple::DriveEventKind::kSpin && off.cycle == 9500 &&
                       !off.motor_on && off.spin_until == 9500 + tisple::kMotorOffDelay);
}

// the far stop: past half-track 79 the head cannot go either
void TestFarStop(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> disk = NoiseDisk(32);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*disk);
    uint64_t t = 100;
    Access(*a, 0x9, t);
    for (int h = 0; h < tisple::kDiskHalfTracks - 1; h++) {
        StepUp(*a, h, t);
    }
    uint64_t steps = a->slots.disk2.log_count;
    StepUp(*a, tisple::kDiskHalfTracks - 1, t);
    CHECK(results, a->slots.disk2.drives[0].half_track == tisple::kDiskHalfTracks - 1);
    CHECK(results, a->slots.disk2.log_count == steps + 1);
    CHECK(results, Event(*a, steps).kind == tisple::DriveEventKind::kStop);
}

// our boot rom walks the head 80 phases down from track 0: at the stop each four phases make
// a knock (pulled outward), nothing (opposite), and a step in and back out
void TestBootRecalibration(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> disk = NoiseDisk(33);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*disk);
    tisple::StartDiskBoot(*a);
    tisple::RunApple2(*a, 20000);
    const tisple::Disk2State& card = a->slots.disk2;
    int knocks = 0;
    int steps = 0;
    for (uint64_t n = 0; n < card.log_count; n++) {
        knocks += Event(*a, n).kind == tisple::DriveEventKind::kStop ? 1 : 0;
        steps += Event(*a, n).kind == tisple::DriveEventKind::kStep ? 1 : 0;
    }
    CHECK(results, knocks == 20 && steps == 40);
    CHECK(results, card.drives[0].half_track == 0);
}

std::vector<int16_t> Render(tisple::DriveSoundRenderer& r, const tisple::Apple2& a,
                            uint64_t cycle) {
    std::vector<int16_t> out;
    tisple::MixDriveSound(r, a, cycle, out, 0);
    return out;
}

bool AllZero(const std::vector<int16_t>& samples, size_t from, size_t to) {
    for (size_t i = from; i < to && i < samples.size(); i++) {
        if (samples[i] != 0) {
            return false;
        }
    }
    return true;
}

double Rms(const std::vector<int16_t>& samples, size_t from, size_t to) {
    double sum = 0;
    for (size_t i = from; i < to; i++) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    return std::sqrt(sum / static_cast<double>(to - from)) / 32768.0;
}

// silent with the drive still; the motor whirs while it spins and stops after the off delay
void TestMotorSound(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> disk = NoiseDisk(34);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*disk);
    tisple::DriveSoundRenderer r;
    tisple::StartDriveSound(r, *a);
    uint64_t second = static_cast<uint64_t>(tisple::kCpuHz);
    std::vector<int16_t> quiet = Render(r, *a, second / 2);
    CHECK(results, !quiet.empty() && AllZero(quiet, 0, quiet.size()));
    Access(*a, 0x9, second / 2);
    Access(*a, 0x8, second);
    a->cpu.cycles = 4 * second;
    std::vector<int16_t> out = Render(r, *a, 4 * second);
    size_t rate = tisple::kAudioRate;
    CHECK(results, r.samples_done == tisple::SamplesForCycles(4 * second));
    // at speed, a whir well under the speaker's 0.35; quiet once it has run down
    double spinning = Rms(out, rate / 4, rate / 2);
    CHECK(results, spinning > 0.003 && spinning < 0.05);
    CHECK(results, Rms(out, 0, 100) < spinning);                  // still ramping up
    CHECK(results, Rms(out, rate * 3 / 2, rate * 7 / 4) > 0.003); // off, but within the delay
    CHECK(results, AllZero(out, rate * 7 / 2, out.size()));
}

// a step sounds from the sample its cycle falls in, and not before
void TestStepTiming(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> disk = NoiseDisk(35);
    std::unique_ptr<tisple::Apple2> with = MachineWithDisk(*disk);
    std::unique_ptr<tisple::Apple2> without = MachineWithDisk(*disk);
    tisple::DriveSoundRenderer r1;
    tisple::DriveSoundRenderer r2;
    tisple::StartDriveSound(r1, *with);
    tisple::StartDriveSound(r2, *without);
    Access(*with, 0x9, 1000);
    Access(*without, 0x9, 1000);
    uint64_t step = 300007;
    Access(*with, 0x3, step); // phase 1: up a half-track
    CHECK(results, with->slots.disk2.drives[0].half_track == 1);
    uint64_t end = 600000;
    with->cpu.cycles = end;
    without->cpu.cycles = end;
    std::vector<int16_t> a = Render(r1, *with, end);
    std::vector<int16_t> b = Render(r2, *without, end);
    CHECK(results, a.size() == b.size());
    size_t first = 0;
    while (first < a.size() && a[first] == b[first]) {
        first++;
    }
    CHECK(results, first == static_cast<size_t>(static_cast<double>(step) / kCyclesPerSample));
    int peak = 0;
    for (size_t i = first; i < first + 200 && i < a.size(); i++) {
        int d = std::abs(a[i] - b[i]);
        peak = d > peak ? d : peak;
    }
    CHECK(results, peak > 1000 && peak < 32767 / 3); // heard, below the speaker
    CHECK(results, r1.impacts == 1 && r1.late == 0 && r1.dropped == 0);
}

// the head's pace: knocks as fast as Apple's rom makes them sound at once; our rom's 80
// phases in 2 ms wait their turn
void TestHeadPace(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> disk = NoiseDisk(36);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*disk);
    tisple::DriveSoundRenderer r;
    tisple::StartDriveSound(r, *a);
    uint64_t t = 100;
    Access(*a, 0x9, t);
    // Apple's rom: a phase every 20 ms, from phase 3 down
    for (int i = 0; i < 8; i++) {
        int phase = (3 - i) & 3;
        Access(*a, phase * 2 + 1, t += 20200);
        Access(*a, phase * 2, t + 10);
    }
    a->cpu.cycles = t + 20000;
    Render(r, *a, t + 20000);
    CHECK(results, r.impacts == 6 && r.late == 0); // knock, step in, step out, twice

    std::unique_ptr<tisple::Apple2> b = MachineWithDisk(*disk);
    tisple::StartDriveSound(r, *b);
    tisple::StartDiskBoot(*b);
    tisple::RunApple2(*b, 20000); // the whole recalibration
    uint64_t now = tisple::Cycles(*b);
    Render(r, *b, now);
    CHECK(results, r.impacts == 1); // the first knock; the rest are queued
    tisple::RunApple2(*b, static_cast<uint64_t>(tisple::kCpuHz * 2));
    Render(r, *b, tisple::Cycles(*b));
    CHECK(results, r.impacts >= 60 && r.late >= 59 && r.dropped == 0);
}

// the drives' sound adds into samples already there
void TestMixesIn(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> disk = NoiseDisk(37);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*disk);
    tisple::DriveSoundRenderer r;
    tisple::StartDriveSound(r, *a);
    a->cpu.cycles = 10000;
    std::vector<int16_t> out = {7};
    size_t count = tisple::SamplesForCycles(10000);
    out.resize(1 + count, 100);
    tisple::MixDriveSound(r, *a, 10000, out, 1);
    CHECK(results, out.size() == 1 + count && out[0] == 7 && out[1] == 100 && out[count] == 100);
}

} // namespace

void RunDiskSoundTests(TestResults& results) {
    TestCardEvents(results);
    TestFarStop(results);
    TestBootRecalibration(results);
    TestMotorSound(results);
    TestStepTiming(results);
    TestHeadPace(results);
    TestMixesIn(results);
}

} // namespace tisple_tests
