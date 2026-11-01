#pragma once

// lib/save's writer: our own 6502 code that writes 256-byte pieces of main memory to the
// sectors a disk table's record names (disk_tables.hpp), on the disk the program booted from.
// License: MIT as well as GPL-3.0-or-later, like the boot loader (boot_loader.hpp): it is
// copied onto users' disks. lib/save keeps room for it (save.writer) and tisple build --disk
// loads it there with the program (BootExtras), built for that disk: it refuses any track
// below the floor (the lowest track a disk table takes, so never the loader's, the program's
// or the catalog's) and reads its slot from lib/disk's reader.
//
// memory, origin page-aligned, 1.5 KB:
//   origin         the entries, the variables, the code, the tables
//   origin + $5AA  the 86 low-bit nibbles of the sector being written (to the page's end), the
//                  checksum nibble just before
//   lib/disk's bounce page (disk.work, the reader's origin - $300): its 256 high-six-bit
//                  nibbles; the reader only uses that page while it reads
// entries:
//   origin + 0  begin. In: $FA/$FB the record, $FC/$FD the first piece's bytes (main memory;
//               each piece is the next 256). Checks the card in the slot the disk booted from,
//               every track the record names against the floor, turns the motor on (drive 1)
//               and looks at the write protect. Gives A = kWriterMore, kWriterDone for an empty
//               record, or kWriterNoCard, kWriterRefused, kWriterProtected (motor off).
//   origin + 3  next: writes the next piece: its 343 nibbles worked out first, then the head
//               to its track, then its sector's data field written right after its address
//               field. kWriterMore while pieces are left, kWriterDone after the last (motor
//               off), or an error (motor off): kWriterNoDisk, kWriterBadSector (its address
//               field not seen in four turns), kWriterBadTrack (five seeks).
// timing: the data field is written as DOS 3.3's RWTS writes it: five self-sync bytes (40
// cycles each), D5 AA AD, 343 nibbles, DE AA EB, one more $FF, a nibble every 32 cycles.
// interrupts are off (SEI) from the address field's first mark to the end of the data field,
// about 12,000 cycles; the caller's I flag is given back otherwise.

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

constexpr int kWriterBegin = 0;
constexpr int kWriterNext = 3;
constexpr int kWriterBytes = 0x600; // what lib/save keeps for it
constexpr int kWriterLow = 0x5AA;   // the 86 low nibbles
constexpr int kWriterSum = 0x5A9;   // the checksum nibble: the code and tables end before it

// what begin and next give in A (lib/save has the same numbers; 0-5 as the reader's)
constexpr std::uint8_t kWriterDone = 0;
constexpr std::uint8_t kWriterMore = 1;
constexpr std::uint8_t kWriterNoCard = 2;
constexpr std::uint8_t kWriterNoDisk = 3;
constexpr std::uint8_t kWriterBadSector = 4;
constexpr std::uint8_t kWriterBadTrack = 5;
constexpr std::uint8_t kWriterProtected = 6; // the disk's write protect is on
constexpr std::uint8_t kWriterRefused = 7;   // the record names a track below the floor

// the writer's bytes for origin (page-aligned, $0400-$BA00): floor the lowest track it may
// write (1-34), reader lib/disk's reader's origin (the boot slot is kept there, its bounce
// page below)
bool BuildDiskWriter(std::uint16_t origin, int floor, std::uint16_t reader,
                     std::vector<std::uint8_t>& out, std::string& error);

} // namespace tisple
