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

#include "check/program.hpp"
#include "disk/nibble.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

constexpr std::uint32_t kMaxDiskTableBytes = 0xC000; // more than any memory holds at once
constexpr int kDefaultInterleave = 2;

inline std::uint32_t DiskTableSectors(std::uint32_t bytes) {
    return (bytes + 0xFF) / 0x100;
}

inline std::uint32_t DiskTableRecordSize(std::uint32_t bytes) {
    return 3 + DiskTableSectors(bytes);
}

// fills each table's tracks and sectors; false when they do not all fit on tracks 1-34
bool PlaceDiskTables(std::vector<DiskTable>& tables, std::string& error);

// the lowest track the tables use, 0 without tables
int DiskTablesFirstTrack(const std::vector<DiskTable>& tables);

// the record of a placed table
std::vector<std::uint8_t> DiskTableRecord(const DiskTable& table);

// the placed tables' bytes into their sectors (zeros after a last part sector)
void WriteDiskTables(const std::vector<DiskTable>& tables, DiskSectors& disk);

} // namespace tisple
