#pragma once

// lib/disk's reader in pieces (disk_reader.cpp, reader_read.cpp): where it keeps things and
// the labels its routines share. License: MIT as well as GPL-3.0-or-later (disk_reader.hpp).

#include "disk/disk_reader.hpp"
#include "disk/loader_code.hpp"

#include <cstdint>

namespace tisple {

// the variables' offsets from the origin, after the two jumps
enum ReaderVar : int {
    kRvSlot = kReaderSlot,
    kRvHead, // the head's half-track, $FF until an address field says
    kRvTarget,
    kRvNextHalf,
    kRvWait,
    kRvMode, // 0 main memory, 1 aux
    kRvDstLo,
    kRvDstHi,
    kRvLenLo,
    kRvLenHi,
    kRvFirst,   // the table's first track
    kRvSectors, // its 256-byte pieces
    kRvLast,    // bytes in the last piece, 0 for 256
    kRvEntry,   // the next piece no track has asked for yet
    kRvGot,     // pieces in
    kRvTrack,   // the track being read
    kRvNeed,    // its pieces not in yet
    kRvFields,  // address fields seen in this call
    kRvSeeks,   // seeks to this track
    kRvFlags,   // the caller's P: its I flag goes back between sectors
    kRvCountLo, // nibbles left before an address search gives up
    kRvCountHi,
    kRvField, // 4: the address field, last first: checksum, sector, track, volume
    kRvTemp = kRvField + 4,
    kRvReady,  // the tables are built
    kRvPiece,  // the piece being placed
    kRvOffset, // a track's offset from the first, high four bits of its record entries
    kRvResult,
    kRvCode, // the code follows
};

struct Reader {
    Loader l; // the boot loader's writer: its seek, wait and patched latch reads
    int origin = 0;
    int bounce = 0;
    int page_a = 0; // T2, the entries and the high values' nibble table
    int page_b = 0; // T3, U, want and the low entries' nibble table

    int Var(int offset) const {
        return origin + offset;
    }
    int T2() const {
        return page_a;
    }
    // the sector's 86 low-bit entries, pairs swapped: up to the nibble table's first, $96
    int Entries() const {
        return page_a + 0x40;
    }
    // by physical sector, the piece it holds, $FF when not wanted
    int Want() const {
        return page_b + 0x80;
    }
    int HighTable() const {
        return page_a; // + nibble
    }
    int T3() const {
        return page_b;
    }
    int U() const {
        return page_b + 0x40;
    }
    int LowTable() const {
        return page_b; // + nibble
    }

    // the caller's I flag back (A and the flags change)
    void RestoreI() {
        l.c.Mem(Mn::LDA, Var(kRvFlags));
        l.c.Op(Mn::PHA);
        l.c.Op(Mn::PLP);
    }
};

struct ReaderLabels {
    std::int32_t begin = -1;
    std::int32_t next = -1;
    std::int32_t seek = -1;
    std::int32_t wait = -1;
    std::int32_t address = -1;
    std::int32_t data = -1;
    std::int32_t fix = -1;
    std::int32_t place = -1;
    std::int32_t tables = -1;
    std::int32_t track_setup = -1;
    // instructions begin patches: the record entries' LDA abs,Y (three of them), the slot
    // rom's signature reads (three)
    std::int32_t entry_reads[3] = {-1, -1, -1};
    std::int32_t signature[3] = {-1, -1, -1};
};

// reader_read.cpp: the address and data fields, the fix of a sector into place, the tables
void EmitReaderAddress(Reader& r, std::int32_t address);
void EmitReaderData(Reader& r, std::int32_t data);
void EmitReaderFix(Reader& r, std::int32_t fix, std::int32_t s0, std::int32_t s86,
                   std::int32_t s172, std::int32_t t0, std::int32_t t86);
void EmitReaderTables(Reader& r, std::int32_t tables);

} // namespace tisple
