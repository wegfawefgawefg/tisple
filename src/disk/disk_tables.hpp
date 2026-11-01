#pragma once

// disk tables: const tables placed :on disk live on the self-booting disk, not in memory, and
// lib/disk reads them in while the program runs (docs/disks.md, Disk tables).
//
// where they go: in declaration order on the last tracks of the disk, as high as they fit
// (track 34 down, the catalog's track 17 skipped), so their places are known while checking,
// before the program's size is: the program fills tracks 1 on, and the disk build fails if
// the two meet. A table starts at the sector after the one before it ends, on the same track
// when there is room. Its sectors go round a track `interleave` physical sectors apart (2 by
// default: 0, 2, ... 14, 1, 3, ... 15), the next free one when that is taken.
//
// the record, the bytes the table's name holds in memory:
//   0-1  the table's length in bytes, low byte first
//   2    the first track
//   3... one byte a 256-byte piece, in order: its track less the first one (high four bits)
//        and its physical sector (low four bits)
//
// tables :on disk2 go on the second disk, which holds nothing else: one after another from
// track 1 (track 0 has a boot sector that says which disk it is), every other physical sector
// (0, 2, ... 14, 1, 3, ... 15) as the slots of one long row, so a table is its first slot and
// its length. Their record is four bytes, whatever the table's size:
//   0-1  the length, low byte first
//   2    the first track, plus $80: on the second disk, its pieces in the row
//   3    the first piece's slot on that track (0-15: physical sector 2n, or 2n - 15 from 8)

#include "check/program.hpp"
#include "disk/nibble.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

constexpr std::uint32_t kMaxDiskTableBytes = 0xC000; // more than any memory holds at once
constexpr int kDefaultInterleave = 2;
constexpr std::uint8_t kDisk2Record = 0x80; // the first track's byte of a table on disk 2
constexpr int kDisk2FirstTrack = 1;         // track 0: the boot sector that names the disk

inline std::uint32_t DiskTableSectors(std::uint32_t bytes) {
    return (bytes + 0xFF) / 0x100;
}

inline std::uint32_t DiskTableRecordSize(std::uint32_t bytes, int disk = 1) {
    return disk == 2 ? 4 : 3 + DiskTableSectors(bytes);
}

// physical sector of slot n (0-15) on a disk-2 track: every other one, round twice
inline int Disk2Sector(int slot) {
    return ((slot << 1) & 15) | (slot >> 3);
}

// fills each table's tracks and sectors; false when they do not all fit: the boot disk's on
// tracks 1-34 (but 17), the second disk's on tracks 1-34
bool PlaceDiskTables(std::vector<DiskTable>& tables, std::string& error);

// the lowest track the boot disk's tables use, 0 without tables there
int DiskTablesFirstTrack(const std::vector<DiskTable>& tables);

// whether any table is on the second disk; and the tracks they take there (the last, 0 for none)
bool HasDisk2Tables(const std::vector<DiskTable>& tables);
int Disk2LastTrack(const std::vector<DiskTable>& tables);

// the record of a placed table
std::vector<std::uint8_t> DiskTableRecord(const DiskTable& table);

// the placed tables of disk (1 or 2) into their sectors (zeros after a last part sector)
void WriteDiskTables(const std::vector<DiskTable>& tables, DiskSectors& disk, int which = 1);

// the second disk: its tables, and on track 0 a boot sector that says to boot the first
void MakeDisk2(const std::vector<DiskTable>& tables, DiskSectors& disk);

} // namespace tisple
