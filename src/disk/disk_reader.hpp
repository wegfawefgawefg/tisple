#pragma once

// lib/disk's reader: our own 6502 code that reads a disk table (disk_tables.hpp) into main or
// aux memory while the program runs, a sector at a time, so the program can do its own work
// between sectors. License: MIT as well as GPL-3.0-or-later, like the boot loader
// (boot_loader.hpp): it is copied onto users' disks. lib/disk keeps room for it (disk.reader,
// and disk.work for the three pages below), and tisple build --disk loads it there with the
// program (BootExtras).
//
// memory, origin page-aligned:
//   origin - $300  the bounce page: each sector's 256 high-six-bit values, shifted up two
//   origin - $200  T2 (+$00): an entry's pair for the second third of the bytes; the sector's
//                  86 low-bit entries (+$40-$95); the nibble table for the 256 values, nibble
//                  n's value times 4 at + n ($96-$FF)
//   origin - $100  T3 (+$00), the pair for the third third; U (+$40), swapped pairs back to
//                  the value times 4; want (+$80, 16 bytes); the nibble table for the 86
//                  low-bit entries, each value with its pairs of bits swapped
//   origin         the code's entries, its variables, then the code
// entries:
//   origin + 0  begin. In: X = 0 to read into main memory, 1 aux; $FA/$FB the table's record,
//               $FC/$FD where its bytes go (lib/auxmem's from and to). Motor on, drive 1.
//               Gives A = kReaderMore, or kReaderDone for an empty table, or kReaderNoCard.
//   origin + 3  next: reads the next sector of the table, whichever of the track's wanted ones
//               comes under the head first, seeking to its track first if need be. Gives
//               kReaderMore while sectors are left, kReaderDone after the last (motor off), or
//               an error (motor off).
//   origin + 6  the slot times 16: the boot loader writes it (LoaderPlan::slot_at); 0 is 6
// interrupts: next turns them off (SEI) from an address field's first mark to the end of its
// data field, about 12,000 cycles, and while it moves a sector into aux memory (RAMWRT on),
// and gives back the caller's I flag between: a tick waits, none is lost below about 80 Hz.

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

constexpr int kReaderBegin = 0;
constexpr int kReaderNext = 3;
constexpr int kReaderSlot = 6;
constexpr int kReaderWork = 0x300; // below the origin: the bounce page and the tables

// what begin and next give in A (lib/disk has the same numbers)
constexpr std::uint8_t kReaderDone = 0;
constexpr std::uint8_t kReaderMore = 1;
constexpr std::uint8_t kReaderNoCard = 2;    // no disk ii card in the slot
constexpr std::uint8_t kReaderNoDisk = 3;    // no address field read for three turns
constexpr std::uint8_t kReaderBadSector = 4; // four turns without a wanted sector reading
constexpr std::uint8_t kReaderBadTrack = 5;  // the head did not get to the track in 4 seeks

// the reader's bytes to load at origin (page-aligned, $0400-$BC00)
bool BuildDiskReader(std::uint16_t origin, std::vector<std::uint8_t>& out, std::string& error);

} // namespace tisple
