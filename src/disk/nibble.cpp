#include "disk/nibble.hpp"

#include <cstring>
#include <vector>

namespace tisple {

namespace {

// sync gaps, in self-sync bytes: before sector 0, after each address field, after each
// data field. the track comes to 51264 cells, close to a real track at 300 rpm
constexpr int kGap1 = 64;
constexpr int kGap2 = 6;
constexpr int kGap3 = 20;

// physical sector -> the sector of the file's track that it holds
const int kDosFileSector[kSectorsPerTrack] = {0, 7, 14, 6, 13, 5, 12, 4, 11, 3, 10, 2, 9, 1, 8, 15};
const int kProdosFileSector[kSectorsPerTrack] = {0, 8,  1, 9,  2, 10, 3, 11,
                                                 4, 12, 5, 13, 6, 14, 7, 15};

bool IsDataNibble(int n) {
    int ones = n & (n << 1) & 0x7E;
    int zeros = ~(n | (n << 1)) & 0x7E;
    return (n & 0x80) != 0 && ones != 0 && (zeros & (zeros - 1)) == 0;
}

struct NibbleTables {
    uint8_t to_nibble[64] = {};
    int8_t to_six[256] = {};
};

NibbleTables BuildTables() {
    NibbleTables t;
    std::memset(t.to_six, -1, sizeof(t.to_six));
    int next = 0;
    for (int n = 0x80; n < 0x100; n++) {
        if (IsDataNibble(n)) {
            t.to_nibble[next] = static_cast<uint8_t>(n);
            t.to_six[n] = static_cast<int8_t>(next);
            next++;
        }
    }
    return t;
}

const NibbleTables kTables = BuildTables();

// two low bits, swapped: what a 6-and-2 low-bits entry holds for one byte
int SwapLow(uint8_t b) {
    return ((b & 1) << 1) | ((b >> 1) & 1);
}

struct TrackWriter {
    DiskTrack* track = nullptr;
    uint32_t pos = 0;

    void Cell(bool one) {
        if (pos >= kMaxTrackBits) {
            return;
        }
        uint8_t mask = static_cast<uint8_t>(0x80 >> (pos & 7));
        if (one) {
            track->bits[pos >> 3] |= mask;
        } else {
            track->bits[pos >> 3] &= static_cast<uint8_t>(~mask);
        }
        pos++;
    }
    void Byte(uint8_t b) {
        for (int i = 7; i >= 0; i--) {
            Cell(((b >> i) & 1) != 0);
        }
    }
    void Sync(int count) {
        for (int i = 0; i < count; i++) {
            Byte(0xFF);
            Cell(false);
            Cell(false);
        }
    }
    void Pair(uint8_t value) {
        uint8_t nibbles[2];
        Encode44(value, nibbles);
        Byte(nibbles[0]);
        Byte(nibbles[1]);
    }
};

bool CellAt(const DiskTrack& t, uint32_t pos) {
    return (t.bits[pos >> 3] & (0x80 >> (pos & 7))) != 0;
}

// the disk bytes an ideal reader sees over two turns from cell 0: each starts at a one
std::vector<uint8_t> ReadNibbles(const DiskTrack& t, uint32_t cells) {
    std::vector<uint8_t> out;
    if (t.bit_count == 0) {
        return out;
    }
    uint32_t pos = 0;
    uint32_t done = 0;
    while (done < cells) {
        uint8_t value = 0;
        int got = 0;
        while (got < 8 && done < cells) {
            bool one = CellAt(t, pos);
            pos = pos + 1 == t.bit_count ? 0 : pos + 1;
            done++;
            if (got == 0 && !one) {
                continue; // between bytes
            }
            value = static_cast<uint8_t>((value << 1) | (one ? 1 : 0));
            got++;
        }
        if (got == 8) {
            out.push_back(value);
        }
    }
    return out;
}

// the index of D5 AA <third> at or after from, or -1
int FindMark(const std::vector<uint8_t>& n, size_t from, size_t limit, uint8_t third) {
    for (size_t i = from; i + 2 < n.size() && i < limit; i++) {
        if (n[i] == 0xD5 && n[i + 1] == 0xAA && n[i + 2] == third) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

int FileSectorOfPhysical(SectorOrder order, int physical) {
    return order == SectorOrder::kDos ? kDosFileSector[physical & 15]
                                      : kProdosFileSector[physical & 15];
}

int DosLogicalOfPhysical(int physical) {
    return kDosFileSector[physical & 15];
}

int PhysicalOfDosLogical(int logical) {
    for (int p = 0; p < kSectorsPerTrack; p++) {
        if (kDosFileSector[p] == (logical & 15)) {
            return p;
        }
    }
    return 0;
}

uint8_t NibbleOf6(int value) {
    return kTables.to_nibble[value & 0x3F];
}

int SixOfNibble(uint8_t nibble) {
    return kTables.to_six[nibble];
}

void Encode44(uint8_t value, uint8_t out[2]) {
    out[0] = static_cast<uint8_t>((value >> 1) | 0xAA);
    out[1] = static_cast<uint8_t>(value | 0xAA);
}

uint8_t Decode44(uint8_t odd, uint8_t even) {
    return static_cast<uint8_t>(((odd << 1) | 1) & even);
}

void Encode62(const uint8_t sector[kSectorBytes], uint8_t out[kEncodedSectorNibbles]) {
    uint8_t values[342];
    for (int k = 0; k < 86; k++) {
        int v = SwapLow(sector[k]) | (SwapLow(sector[k + 86]) << 2);
        if (k + 172 < kSectorBytes) {
            v |= SwapLow(sector[k + 172]) << 4;
        }
        values[k] = static_cast<uint8_t>(v);
    }
    for (int i = 0; i < kSectorBytes; i++) {
        values[86 + i] = static_cast<uint8_t>(sector[i] >> 2);
    }
    uint8_t before = 0;
    for (int i = 0; i < 342; i++) {
        out[i] = NibbleOf6(values[i] ^ before);
        before = values[i];
    }
    out[342] = NibbleOf6(before);
}

bool Decode62(const uint8_t nibbles[kEncodedSectorNibbles], uint8_t sector[kSectorBytes]) {
    uint8_t values[342];
    uint8_t chain = 0;
    for (int i = 0; i < kEncodedSectorNibbles; i++) {
        int six = SixOfNibble(nibbles[i]);
        if (six < 0) {
            return false;
        }
        chain = static_cast<uint8_t>(chain ^ six);
        if (i < 342) {
            values[i] = chain;
        }
    }
    if (chain != 0) {
        return false;
    }
    for (int i = 0; i < kSectorBytes; i++) {
        int k = i % 86;
        int low = (values[k] >> (2 * (i / 86))) & 3;
        sector[i] =
            static_cast<uint8_t>((values[86 + i] << 2) | SwapLow(static_cast<uint8_t>(low)));
    }
    return true;
}

void NibblizeTrack(const uint8_t sectors[kSectorsPerTrack][kSectorBytes], int track, uint8_t volume,
                   DiskTrack& out) {
    std::memset(out.bits, 0, sizeof(out.bits));
    TrackWriter w;
    w.track = &out;
    w.Sync(kGap1);
    for (int p = 0; p < kSectorsPerTrack; p++) {
        uint8_t t = static_cast<uint8_t>(track);
        uint8_t s = static_cast<uint8_t>(p);
        w.Byte(0xD5);
        w.Byte(0xAA);
        w.Byte(0x96);
        w.Pair(volume);
        w.Pair(t);
        w.Pair(s);
        w.Pair(static_cast<uint8_t>(volume ^ t ^ s));
        w.Byte(0xDE);
        w.Byte(0xAA);
        w.Byte(0xEB);
        w.Sync(kGap2);
        w.Byte(0xD5);
        w.Byte(0xAA);
        w.Byte(0xAD);
        uint8_t nibbles[kEncodedSectorNibbles];
        Encode62(sectors[p], nibbles);
        for (uint8_t n : nibbles) {
            w.Byte(n);
        }
        w.Byte(0xDE);
        w.Byte(0xAA);
        w.Byte(0xEB);
        w.Sync(kGap3);
    }
    out.bit_count = w.pos;
}

void TrackFromNibbles(const uint8_t* nibbles, uint32_t count, DiskTrack& out) {
    std::memset(out.bits, 0, sizeof(out.bits));
    TrackWriter w;
    w.track = &out;
    for (uint32_t i = 0; i < count && i < kTrackBytes; i++) {
        w.Byte(nibbles[i]);
    }
    out.bit_count = w.pos;
}

uint32_t TrackNibbles(const DiskTrack& track, uint8_t* out, uint32_t max) {
    std::vector<uint8_t> nibbles = ReadNibbles(track, track.bit_count);
    uint32_t count = 0;
    for (uint8_t n : nibbles) {
        if (count == max) {
            break;
        }
        out[count++] = n;
    }
    return count;
}

void DenibblizeTrack(const DiskTrack& track, int expect_track,
                     uint8_t sectors[kSectorsPerTrack][kSectorBytes],
                     bool found[kSectorsPerTrack]) {
    for (int p = 0; p < kSectorsPerTrack; p++) {
        found[p] = false;
    }
    // two turns, so a sector across the seam is seen whole once
    std::vector<uint8_t> n = ReadNibbles(track, track.bit_count * 2);
    size_t at = 0;
    for (;;) {
        int mark = FindMark(n, at, n.size(), 0x96);
        if (mark < 0 || static_cast<size_t>(mark) + 11 > n.size()) {
            return;
        }
        size_t i = static_cast<size_t>(mark) + 3;
        uint8_t fields[4];
        for (int f = 0; f < 4; f++) {
            fields[f] = Decode44(n[i + 2 * f], n[i + 2 * f + 1]);
        }
        at = i + 8;
        int sector = fields[2];
        bool good = (fields[0] ^ fields[1] ^ fields[2] ^ fields[3]) == 0 && sector < 16 &&
                    (expect_track < 0 || fields[1] == expect_track);
        // the data field follows within a few dozen bytes, before any other address field
        int data = FindMark(n, at, at + 48, 0xAD);
        int next_address = FindMark(n, at, at + 48, 0x96);
        if (!good || found[sector] || data < 0 || (next_address >= 0 && next_address < data) ||
            static_cast<size_t>(data) + 3 + kEncodedSectorNibbles > n.size()) {
            continue;
        }
        uint8_t bytes[kSectorBytes];
        if (Decode62(&n[static_cast<size_t>(data) + 3], bytes)) {
            std::memcpy(sectors[sector], bytes, kSectorBytes);
            found[sector] = true;
        }
        at = static_cast<size_t>(data) + 3;
    }
}

} // namespace tisple
