#pragma once

// lib/save's writer in pieces (disk_writer.cpp, writer_write.cpp): where it keeps things and
// the labels its routines share. License: MIT as well as GPL-3.0-or-later (disk_writer.hpp).

#include "disk/disk_writer.hpp"
#include "disk/loader_code.hpp"

#include <cstdint>

namespace tisple {

// the variables' offsets from the origin, after the two jumps
enum WriterVar : int {
    kWvSlot = 6, // slot * 16
    kWvHead,     // the head's half-track, $FF until an address field says
    kWvTarget,
    kWvNextHalf,
    kWvWait,
    kWvFirst,  // the record's first track
    kWvPieces, // its 256-byte pieces
    kWvPiece,  // the next one to write
    kWvSrcLo,  // the first piece's bytes
    kWvSrcHi,
    kWvTrack, // the piece's track and physical sector
    kWvSector,
    kWvFields, // address fields seen in this call
    kWvSeeks,  // seeks to this track
    kWvFlags,  // the caller's P: its I flag goes back between fields
    kWvCountLo,
    kWvCountHi,
    kWvField, // 4: the address field, last first: checksum, sector, track, volume
    kWvTemp = kWvField + 4,
    kWvPrev,
    kWvResult,
    kWvCode, // the code follows
};

struct Writer {
    Loader l; // the boot loader's writer: its seek, wait and patched latch reads
    int origin = 0;
    int floor = 1;
    int reader_slot = 0;
    int high = 0; // lib/disk's bounce page, for the high nibbles


    int Var(int offset) const {
        return origin + offset;
    }
    int High() const {
        return high;
    }
    int Low() const {
        return origin + kWriterLow;
    }
    // Low() less 170: indexed by 170-255 it is Low()'s 86 bytes, all in one page
    int LowBase() const {
        return origin + kWriterLow - 170;
    }
    int Sum() const {
        return origin + kWriterSum;
    }
    // the caller's I flag back (A and the flags change)
    void RestoreI() {
        l.c.Mem(Mn::LDA, Var(kWvFlags));
        l.c.Op(Mn::PHA);
        l.c.Op(Mn::PLP);
    }
};

struct WriterLabels {
    std::int32_t begin = -1;
    std::int32_t next = -1;
    std::int32_t seek = -1;
    std::int32_t wait = -1;
    std::int32_t address = -1;
    std::int32_t field = -1;
    std::int32_t prenibble = -1;
    std::int32_t nibbles = -1; // the 64 data nibbles, in order
    std::int32_t swaps = -1;   // 12 bytes: a byte's low two bits swapped, at <<0, <<2, <<4
    // patched by begin: the record entries' LDA abs,Y (two), the slot rom's signature reads
    std::int32_t entry_reads[2] = {-1, -1};
    std::int32_t signature[3] = {-1, -1, -1};
    // patched by next: the prenibble's loads of the piece's bytes, and one of them less one
    std::int32_t source[6] = {-1, -1, -1, -1, -1, -1};
    std::int32_t source_back = -1;
};

// writer_write.cpp: the piece's nibbles, the address field and the data field written after it
void EmitWriterPrenibble(Writer& w, const WriterLabels& at);
void EmitWriterAddress(Writer& w, std::int32_t address);
void EmitWriterField(Writer& w, std::int32_t field);
// n cycles of nothing (NOPs, a JMP to the next instruction for an odd n; n is 0 or 2 up)
void EmitPad(Code6502& c, int cycles);

} // namespace tisple
