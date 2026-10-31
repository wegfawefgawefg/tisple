// the disk ii card: see disk2.hpp for the model. adapted from AppleWin's Disk.cpp
// (GPL-2.0-or-later): the stepper rule (ControlStepperDeferred), motor-off clearing the
// magnets, the read latch's hold after a nibble (DataLatchReadWOZ: 7 microseconds, extended
// while zeros follow), the head window's made-up bits, write protect saturating the latch, and
// the one-second spin-down.

#include "machine/disk2.hpp"

#include "machine/bus.hpp"
#include "machine/slots.hpp"

#include <cstring>

namespace tisple {

namespace {

constexpr int8_t kLatchHold = 7; // microseconds a nibble stays after it completes

Disk2State& Card(Apple2& a) {
    return a.slots.disk2;
}

uint64_t AccessCycle(const Apple2& a) {
    return a.cpu.cycles - 1; // the wrapper counts the cycle before performing it
}

// the cycle the selected drive stops turning: now while the motor is on
uint64_t SpinEnd(const Disk2State& card, uint64_t now) {
    if (card.motor_on) {
        return now;
    }
    return card.spin_until < now ? card.spin_until : now;
}

DiskTrack* HeadTrack(DiskDrive& d) {
    if (d.format == DiskFormat::kNone) {
        return nullptr;
    }
    int track = d.half_track / 2;
    if (track >= kDiskTracks || d.tracks[track].bit_count == 0) {
        return nullptr;
    }
    return &d.tracks[track];
}

bool ReadCell(const DiskTrack& t, uint32_t pos) {
    return (t.bits[pos >> 3] & (0x80 >> (pos & 7))) != 0;
}

void WriteCell(DiskTrack& t, uint32_t pos, bool one) {
    uint8_t mask = static_cast<uint8_t>(0x80 >> (pos & 7));
    if (one) {
        t.bits[pos >> 3] |= mask;
    } else {
        t.bits[pos >> 3] &= static_cast<uint8_t>(~mask);
    }
}

// xorshift32; a one about 3 times in 10
bool NoiseBit(Disk2State& card) {
    uint32_t x = card.noise;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    card.noise = x;
    return x % 10 < 3;
}

// one bit cell through the read amplifier and the sequencer's shift register
void ReadOneCell(Disk2State& card, DiskDrive& d, const DiskTrack* t) {
    bool flux = t != nullptr && ReadCell(*t, d.bit_pos);
    d.head_window = static_cast<uint8_t>(((d.head_window << 1) | (flux ? 1 : 0)) & 0x0F);
    bool bit = d.head_window != 0 ? ((d.head_window >> 1) & 1) != 0 : NoiseBit(card);
    card.shift = static_cast<uint8_t>((card.shift << 1) | (bit ? 1 : 0));
    if (card.latch_hold > 0) {
        card.latch_hold = static_cast<int8_t>(card.latch_hold - 4);
        if (card.latch_hold < 0) {
            card.latch_hold = 0;
        }
        if (card.shift == 0) {
            card.latch_hold = static_cast<int8_t>(card.latch_hold + 4); // zeros keep it there
        }
    }
    if (card.latch_hold == 0) {
        card.latch = card.shift;
        if ((card.shift & 0x80) != 0) {
            card.latch_hold = kLatchHold;
            card.shift = 0;
        }
    }
}

void WriteOneCell(Disk2State& card, DiskDrive& d, DiskTrack* t) {
    if (t != nullptr && !d.write_protected) {
        WriteCell(*t, d.bit_pos, (card.shift & 0x80) != 0);
        d.dirty = true;
    }
    card.shift = static_cast<uint8_t>(card.shift << 1);
}

// turns the selected drive's disk to now, running every cell in the current mode
void CatchUp(Disk2State& card, uint64_t now) {
    DiskDrive& d = card.drives[card.drive];
    uint64_t end = SpinEnd(card, now);
    if (end > d.spun_to) {
        uint64_t cells = (end - d.spun_to) / kCyclesPerBitCell;
        d.spun_to += cells * kCyclesPerBitCell;
        DiskTrack* t = HeadTrack(d);
        uint32_t length = t != nullptr ? t->bit_count : kMaxTrackBits;
        // whole turns beyond the first change nothing a program could tell apart
        if (cells > 2ull * length) {
            cells -= ((cells - length) / length) * length;
        }
        for (uint64_t i = 0; i < cells; i++) {
            if (card.q7) {
                WriteOneCell(card, d, t);
            } else if (card.q6) {
                card.latch = d.write_protected ? 0xFF : 0x00;
            } else {
                ReadOneCell(card, d, t);
            }
            d.bit_pos = d.bit_pos + 1 >= length ? 0 : d.bit_pos + 1;
        }
    }
    if (end < now) {
        d.spun_to = now; // stopped: no turning to make up later
    }
}

// every event the drives' sound needs, with the card as it is after it
void LogEvent(Disk2State& card, DriveEventKind kind, uint64_t now) {
    DriveEvent& e = card.log[card.log_count % kDriveLogSize];
    e.cycle = now;
    e.spin_until = card.spin_until;
    e.kind = kind;
    e.drive = card.drive;
    e.half_track = card.drives[card.drive].half_track;
    e.motor_on = card.motor_on;
    card.log_count++;
}

bool Spinning(const Disk2State& card, uint64_t now) {
    return card.motor_on || now < card.spin_until;
}

// the head moved: keep its place along the track as the lengths differ
void MoveHead(DiskDrive& d, int half_track) {
    int old_track = d.half_track / 2;
    d.half_track = static_cast<uint8_t>(half_track);
    int new_track = half_track / 2;
    if (old_track == new_track || d.format == DiskFormat::kNone) {
        return;
    }
    uint32_t old_length = old_track < kDiskTracks && d.tracks[old_track].bit_count != 0
                              ? d.tracks[old_track].bit_count
                              : kMaxTrackBits;
    uint32_t new_length = new_track < kDiskTracks && d.tracks[new_track].bit_count != 0
                              ? d.tracks[new_track].bit_count
                              : kMaxTrackBits;
    d.bit_pos = static_cast<uint32_t>(static_cast<uint64_t>(d.bit_pos) * new_length / old_length);
    if (d.format == DiskFormat::kNib) {
        d.bit_pos &= ~7u; // .nib tracks have no self-sync gaps to fall back into step on
    }
    if (d.bit_pos >= new_length) {
        d.bit_pos = 0;
    }
}

// which way the magnets that are on pull a head at half_track: +1 in, -1 out, 0 neither
int Pull(uint8_t magnets, int half_track) {
    int direction = 0;
    if ((magnets & (1 << ((half_track + 1) & 3))) != 0) {
        direction++;
    }
    if ((magnets & (1 << ((half_track + 3) & 3))) != 0) {
        direction--;
    }
    return direction;
}

void Stepper(Disk2State& card, int reg, uint64_t now) {
    if (!Spinning(card, now)) {
        return; // the drive is not enabled, so its stepper has no power
    }
    int phase = reg >> 1;
    DiskDrive& d = card.drives[card.drive];
    int pulled = Pull(card.magnets, d.half_track);
    if ((reg & 1) != 0) {
        card.magnets = static_cast<uint8_t>(card.magnets | (1 << phase));
    } else {
        card.magnets = static_cast<uint8_t>(card.magnets & ~(1 << phase));
    }
    int direction = Pull(card.magnets, d.half_track);
    int next = d.half_track + direction;
    next = next < 0 ? 0 : next;
    next = next > kDiskHalfTracks - 1 ? kDiskHalfTracks - 1 : next;
    if (next != d.half_track) {
        MoveHead(d, next);
        LogEvent(card, DriveEventKind::kStep, now);
    } else if (direction != 0 && direction != pulled) {
        LogEvent(card, DriveEventKind::kStop, now); // a new pull against a stop: the grind
    }
}

void Motor(Disk2State& card, bool on, uint64_t now) {
    if (!on) {
        card.magnets = 0; // "drives off" clears the stepper's flip-flops
        if (card.motor_on) {
            card.spin_until = now + kMotorOffDelay;
            card.motor_on = false;
            LogEvent(card, DriveEventKind::kSpin, now);
        }
        return;
    }
    if (card.motor_on) {
        return;
    }
    if (!Spinning(card, now)) {
        card.drives[card.drive].spun_to = now; // spins up from here
    }
    card.motor_on = true;
    LogEvent(card, DriveEventKind::kSpin, now);
}

void SelectDrive(Disk2State& card, int drive, uint64_t now) {
    if (drive == card.drive) {
        return;
    }
    card.drive = static_cast<uint8_t>(drive);
    card.drives[drive].spun_to = now; // the other drive stops; this one turns from now
    LogEvent(card, DriveEventKind::kSpin, now);
}

// Q6 and Q7 switch the sequencer's mode; registers C-F
void SequencerSwitch(Disk2State& card, int reg, uint64_t now) {
    bool on = (reg & 1) != 0;
    if (reg < 0x0E) {
        card.q6 = on;
    } else {
        card.q7 = on;
    }
    if (card.q6 && !card.q7 && Spinning(card, now)) {
        card.shift = 0;
        card.latch_hold = 0;
        card.latch = card.drives[card.drive].write_protected ? 0xFF : 0x00;
    }
}

} // namespace

uint8_t Disk2IoAccess(Apple2& a, int reg, bool is_write, uint8_t value, bool& drives) {
    Disk2State& card = Card(a);
    uint64_t now = AccessCycle(a);
    drives = false;
    CatchUp(card, now);
    if (reg < 0x08) {
        Stepper(card, reg, now);
    } else if (reg < 0x0A) {
        Motor(card, reg == 0x09, now);
    } else if (reg < 0x0C) {
        SelectDrive(card, reg - 0x0A, now);
    } else {
        SequencerSwitch(card, reg, now);
    }
    if (is_write) {
        if (card.q6 && card.q7) {
            card.shift = value; // load: the register takes the byte on the bus
        }
        return 0;
    }
    if ((reg & 1) != 0) {
        return 0; // odd addresses leave the bus floating
    }
    drives = true;
    return card.latch;
}

void ResetDisk2(Apple2& a) {
    Disk2State& card = Card(a);
    uint64_t now = a.cpu.cycles;
    CatchUp(card, now);
    Motor(card, false, now);
    SelectDrive(card, 0, now);
    card.q6 = false;
    card.q7 = false;
    card.magnets = 0;
}

void InsertDisk2Card(Apple2& a, int slot, const uint8_t* rom) {
    InsertCard(a, slot, CardKind::kDisk2);
    Disk2State& card = Card(a);
    card.slot = static_cast<uint8_t>(slot);
    if (rom != nullptr) {
        std::memcpy(card.rom, rom, sizeof(card.rom));
    } else {
        BuildDisk2BootRom(slot, card.rom);
    }
}

void InsertDisk(Apple2& a, int drive, DiskFormat format, bool write_protected) {
    DiskDrive& d = Card(a).drives[drive & 1];
    d.format = format;
    d.write_protected = write_protected;
    d.dirty = false;
    d.bit_pos = 0;
    d.head_window = 0;
    d.spun_to = a.cpu.cycles;
}

void EjectDisk(Apple2& a, int drive) {
    DiskDrive& d = Card(a).drives[drive & 1];
    d.format = DiskFormat::kNone;
    d.dirty = false;
    for (DiskTrack& t : d.tracks) {
        t.bit_count = 0;
        std::memset(t.bits, 0, sizeof(t.bits));
    }
}

bool HasDisk(const Apple2& a, int drive) {
    return a.slots.disk2.drives[drive & 1].format != DiskFormat::kNone;
}

void StartDiskBoot(Apple2& a) {
    ResetApple2(a);
    Regs regs;
    regs.pc = static_cast<uint16_t>(0xC000 + a.slots.disk2.slot * 0x100);
    regs.s = 0xFF;
    regs.p = kFlagU | kFlagI;
    SetRegs(a.cpu, regs);
}

int DriveTrack(const Apple2& a, int drive) {
    int track = a.slots.disk2.drives[drive & 1].half_track / 2;
    return track < kDiskTracks ? track : -1;
}

bool DriveSpinning(const Apple2& a, uint64_t cycle) {
    return Spinning(a.slots.disk2, cycle);
}

const DriveEvent& DriveLogEvent(const Apple2& a, uint64_t n) {
    return a.slots.disk2.log[n % kDriveLogSize];
}

} // namespace tisple
