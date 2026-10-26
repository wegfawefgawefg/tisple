#pragma once

// the 16-sector disk format, as DOS 3.3, ProDOS and the boot roms write it. per Beneath
// Apple DOS chapter 3 and the Apple II Reference Manual's Disk II notes:
// - a disk byte ("nibble") has its top bit set, two neighbouring ones among bits 0-6 and
//   at most one pair of neighbouring zeros: 64 of them carry 6 bits each (6-and-2), and
//   $D5 / $AA are kept for the marks.
// - a sector: an address field D5 AA 96, volume, track, sector, checksum (each byte as two
//   4-and-4 nibbles: odd bits then even bits, the gaps filled with ones), DE AA EB; a gap
//   of self-sync bytes; a data field D5 AA AD, 343 nibbles, DE AA EB; another gap.
// - the 343 nibbles: 86 hold the two low bits of every byte (entry i: bytes i, i+86 and
//   i+172, each pair swapped), 256 hold the six high bits, each nibble is its value xor the
//   one before, and the last is the final value as a checksum.
// - self-sync bytes are $FF and two zero cells: 10 cells, which brings any reader into step.
// - sectors go round the track in physical order 0-15. which part of an image file a
//   physical sector holds is the sector order: DOS 3.3 ("skew") or ProDOS.

#include "machine/apple2.hpp"

#include <cstdint>
#include <string>

namespace tisple {

constexpr int kSectorsPerTrack = 16;
constexpr int kSectorBytes = 256;
constexpr int kEncodedSectorNibbles = 343;
constexpr uint8_t kDefaultVolume = 254;

// a whole 140 KB disk as sectors, indexed by track and physical sector
struct DiskSectors {
    uint8_t data[kDiskTracks][kSectorsPerTrack][kSectorBytes] = {};
};

enum class SectorOrder : uint8_t {
    kDos,
    kProdos,
};

// which 256-byte part of a track's 4 KB in the file sits in physical sector p
int FileSectorOfPhysical(SectorOrder order, int physical);
// DOS 3.3's logical sector number in physical sector p, and back
int DosLogicalOfPhysical(int physical);
int PhysicalOfDosLogical(int logical);

// the 64 data nibbles in order; -1 for a byte that is not one
uint8_t NibbleOf6(int value);
int SixOfNibble(uint8_t nibble);

// 4-and-4: the two nibbles of a byte, and back
void Encode44(uint8_t value, uint8_t out[2]);
uint8_t Decode44(uint8_t odd, uint8_t even);

// 6-and-2 over one sector: 256 bytes to 343 nibbles, and back (false on a bad checksum or a
// byte that is no data nibble)
void Encode62(const uint8_t sector[kSectorBytes], uint8_t out[kEncodedSectorNibbles]);
bool Decode62(const uint8_t nibbles[kEncodedSectorNibbles], uint8_t sector[kSectorBytes]);

// a track of 16 sectors as bits, the layout DOS 3.3's INIT writes (gaps of self-sync bytes)
void NibblizeTrack(const uint8_t sectors[kSectorsPerTrack][kSectorBytes], int track, uint8_t volume,
                   DiskTrack& out);
// a track of raw disk bytes (a .nib track) as bits, eight cells each
void TrackFromNibbles(const uint8_t* nibbles, uint32_t count, DiskTrack& out);
// a track as the disk bytes a reader would see from cell 0 on, at most max of them
uint32_t TrackNibbles(const DiskTrack& track, uint8_t* out, uint32_t max);

// the sectors a track holds; found[p] says whether physical sector p read back cleanly.
// expect_track is the track number its address fields should carry (-1: any)
void DenibblizeTrack(const DiskTrack& track, int expect_track,
                     uint8_t sectors[kSectorsPerTrack][kSectorBytes], bool found[kSectorsPerTrack]);

} // namespace tisple
