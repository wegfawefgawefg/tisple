#pragma once

// the disk ii controller card and its two drives, at the bit-cell level.
//
// the card's 16 registers ($C080 + slot * 16), per Sather's "Understanding the Apple IIe"
// chapter 9: 0-7 stepper phases off/on, 8/9 motor off/on, A/B drive 1/2, C/D Q6 off/on,
// E/F Q7 off/on. any access, read or write, flips the switch; only even addresses read the
// data register.
//
// the disk turns under the head one bit cell every 4 cycles whenever the selected drive is
// spinning (motor on, or less than kMotorOffDelay cycles after "off"). the card catches up
// lazily: each access first runs the cells that passed since the last one, in the mode that
// was set then. the sequencer is modelled by its behaviour, not by Apple's P6 state rom:
// - read (Q7 off, Q6 off): each cell shifts the head's bit into the shift register. a full
//   nibble (top bit set) moves to the latch and stays there for 2 more cells, longer while
//   only zeros follow (the sync bytes' gaps), then the latch follows the shift register
//   again. so the cpu sees every nibble for 8 cycles or more, every 32 cycles.
// - sense write protect (Q7 off, Q6 on): the latch reads $FF if the disk is protected, else 0.
// - write (Q7 on): each cell writes the register's top bit and shifts left (zeros in); a cpu
//   write while Q6 is on loads the register at once. a cpu loop that loads every 32 cycles
//   writes plain nibbles; every 40 cycles, 10-bit self-sync bytes.
// - after more than three cells without flux the read amplifier makes up bits (30% ones), as
//   AppleWin models the MC3470 after the WOZ notes; a fixed LFSR keeps runs reproducible.
// - the stepper moves the head one half-track toward a magnet that is next to the head's own
//   (AppleWin's model without quarter tracks), at once, and only while the drive spins. motor
//   off turns every magnet off.
// - every head step, every pull against a stop, and every change of the motor or the selected
//   drive goes in the card's event log (DriveEvent) with its cycle, for the drives' sound
//   (drive_audio.hpp). a pull against a stop is a magnet change that newly asks for a step the
//   head cannot take: the boot's recalibration, which steps outward 40 tracks from anywhere.
// adapted from AppleWin's Disk.cpp (GPL-2.0-or-later; see third_party/applewin/README.md).

#include "machine/apple2.hpp"

#include <cstdint>

namespace tisple {

// the slot hooks; slots.cpp calls these for a kDisk2 card
uint8_t Disk2IoAccess(Apple2& a, int reg, bool is_write, uint8_t value, bool& drives);
void ResetDisk2(Apple2& a); // the reset line: every switch off, drive 1 selected

// our own boot rom for a card in this slot (disk2_rom.cpp has its interface)
void BuildDisk2BootRom(int slot, uint8_t out[256]);

// plugs the card into a slot with a 256-byte boot rom: Apple's DISK2.rom, or null for ours
void InsertDisk2Card(Apple2& a, int slot, const uint8_t* rom);
// the drive's tracks must already hold the disk (src/disk/image.hpp fills them)
void InsertDisk(Apple2& a, int drive, DiskFormat format, bool write_protected);
void EjectDisk(Apple2& a, int drive);
bool HasDisk(const Apple2& a, int drive);

// the no-rom start of a disk boot: reset, then run the card's rom at $Cn00 as the
// autostart rom would, with the stack at $FF and interrupts off
void StartDiskBoot(Apple2& a);

// the track the head is over (the half-track rounded down), or -1 past the last one
int DriveTrack(const Apple2& a, int drive);
bool DriveSpinning(const Apple2& a, uint64_t cycle);

// event n of the card's log, for n in [log_count - kDriveLogSize, log_count)
const DriveEvent& DriveLogEvent(const Apple2& a, uint64_t n);

} // namespace tisple
