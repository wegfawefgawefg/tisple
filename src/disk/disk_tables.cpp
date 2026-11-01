// disk tables' places on the disks, their records, and their bytes on the disks
// (disk_tables.hpp)

#include "disk/disk_tables.hpp"

#include "disk/boot_loader.hpp"

#include <cstring>

namespace tisple {

namespace {

int NextTableTrack(int track) {
    track++;
    return track == kBootCatalogTrack ? track + 1 : track;
}

// every boot disk table laid out from track start on; false when they go past the last track
bool LayFrom(std::vector<DiskTable*>& tables, int start) {
    int track = start;
    bool used[kSectorsPerTrack] = {};
    int filled = 0;
    int last = -1; // the physical sector of the piece before, on this track
    for (DiskTable* table : tables) {
        table->tracks.clear();
        table->sectors.clear();
        std::uint32_t pieces = DiskTableSectors(static_cast<std::uint32_t>(table->bytes.size()));
        for (std::uint32_t k = 0; k < pieces; k++) {
            if (filled == kSectorsPerTrack) {
                track = NextTableTrack(track);
                std::memset(used, 0, sizeof(used));
                filled = 0;
                last = -1;
            }
            if (track > kBootLastTrack) {
                return false;
            }
            int p = last < 0 ? 0 : (last + table->interleave) % kSectorsPerTrack;
            while (used[p]) {
                p = (p + 1) % kSectorsPerTrack;
            }
            used[p] = true;
            filled++;
            last = p;
            table->tracks.push_back(static_cast<std::uint8_t>(track));
            table->sectors.push_back(static_cast<std::uint8_t>(p));
        }
    }
    return true;
}

void Unplace(std::vector<DiskTable*>& tables) {
    for (DiskTable* table : tables) {
        table->tracks.clear();
        table->sectors.clear();
    }
}

// the boot disk's tables on its last tracks, as high as they fit
bool PlaceBootTables(std::vector<DiskTable*>& tables, std::string& error) {
    if (tables.empty()) {
        return true;
    }
    std::uint32_t pieces = 0;
    for (const DiskTable* table : tables) {
        pieces += DiskTableSectors(static_cast<std::uint32_t>(table->bytes.size()));
    }
    for (int start = kBootLastTrack; start >= kBootFirstTrack; start--) {
        if (start != kBootCatalogTrack && LayFrom(tables, start)) {
            return true;
        }
    }
    Unplace(tables);
    error = "the tables :on disk are " + std::to_string(pieces) +
            " sectors; a boot disk holds 528 besides the loader (tracks 1-34 but 17)";
    return false;
}

// the second disk's tables: one row of slots from track 1, every other sector round a track
bool PlaceDisk2Tables(std::vector<DiskTable*>& tables, std::string& error) {
    int slot = kDisk2FirstTrack * kSectorsPerTrack;
    std::uint32_t pieces = 0;
    for (DiskTable* table : tables) {
        table->tracks.clear();
        table->sectors.clear();
        std::uint32_t n = DiskTableSectors(static_cast<std::uint32_t>(table->bytes.size()));
        pieces += n;
        for (std::uint32_t k = 0; k < n; k++, slot++) {
            table->tracks.push_back(static_cast<std::uint8_t>(slot / kSectorsPerTrack));
            table->sectors.push_back(
                static_cast<std::uint8_t>(Disk2Sector(slot % kSectorsPerTrack)));
        }
    }
    if (slot > (kBootLastTrack + 1) * kSectorsPerTrack) {
        Unplace(tables);
        error = "the tables :on disk2 are " + std::to_string(pieces) +
                " sectors; the second disk holds 544 (tracks 1-34)";
        return false;
    }
    return true;
}

} // namespace

bool PlaceDiskTables(std::vector<DiskTable>& tables, std::string& error) {
    std::vector<DiskTable*> boot;
    std::vector<DiskTable*> second;
    for (DiskTable& table : tables) {
        if (table.bytes.empty() || table.bytes.size() > kMaxDiskTableBytes) {
            error = "a table :on disk holds 1 to " + std::to_string(kMaxDiskTableBytes) +
                    " bytes, not " + std::to_string(table.bytes.size());
            return false;
        }
        (table.disk == 2 ? second : boot).push_back(&table);
    }
    return PlaceBootTables(boot, error) && PlaceDisk2Tables(second, error);
}

int DiskTablesFirstTrack(const std::vector<DiskTable>& tables) {
    for (const DiskTable& table : tables) {
        if (table.disk != 2 && !table.tracks.empty()) {
            return table.tracks[0];
        }
    }
    return 0;
}

bool HasDisk2Tables(const std::vector<DiskTable>& tables) {
    for (const DiskTable& table : tables) {
        if (table.disk == 2) {
            return true;
        }
    }
    return false;
}

int Disk2LastTrack(const std::vector<DiskTable>& tables) {
    int last = 0;
    for (const DiskTable& table : tables) {
        if (table.disk == 2 && !table.tracks.empty()) {
            last = table.tracks.back();
        }
    }
    return last;
}

std::vector<std::uint8_t> DiskTableRecord(const DiskTable& table) {
    std::uint32_t size = static_cast<std::uint32_t>(table.bytes.size());
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(size & 0xFF));
    out.push_back(static_cast<std::uint8_t>(size >> 8));
    int first = table.tracks.empty() ? 0 : table.tracks[0];
    if (table.disk == 2) {
        int slot = 0;
        while (slot < kSectorsPerTrack && !table.sectors.empty() &&
               Disk2Sector(slot) != table.sectors[0]) {
            slot++;
        }
        out.push_back(static_cast<std::uint8_t>(first | kDisk2Record));
        out.push_back(static_cast<std::uint8_t>(slot));
        return out;
    }
    out.push_back(static_cast<std::uint8_t>(first));
    for (std::size_t k = 0; k < table.tracks.size(); k++) {
        int offset = table.tracks[k] - first;
        out.push_back(static_cast<std::uint8_t>((offset << 4) | table.sectors[k]));
    }
    return out;
}

void WriteDiskTables(const std::vector<DiskTable>& tables, DiskSectors& disk, int which) {
    for (const DiskTable& table : tables) {
        if ((table.disk == 2) != (which == 2)) {
            continue;
        }
        for (std::size_t k = 0; k < table.tracks.size(); k++) {
            std::uint8_t* sector = disk.data[table.tracks[k]][table.sectors[k]];
            std::memset(sector, 0, kSectorBytes);
            std::size_t from = k * kSectorBytes;
            std::size_t count = table.bytes.size() - from;
            count = count > kSectorBytes ? kSectorBytes : count;
            std::memcpy(sector, table.bytes.data() + from, count);
        }
    }
}

} // namespace tisple

namespace tisple {

namespace {

// the second disk's boot sector: booted by mistake, it clears the text screen and says which
// disk to boot, then waits there. One sector at $0800 (its first byte), plain 6502, no rom
std::vector<std::uint8_t> Disk2BootSector() {
    const char* say = "THIS IS DISK 2: BOOT DISK 1";
    int length = static_cast<int>(std::strlen(say));
    std::uint16_t row12 = 0x0628 + static_cast<std::uint16_t>((40 - length) / 2);
    std::vector<std::uint8_t> c = {
        0x01, // one sector
        0xA9,
        0xA0, // LDA #$A0 (a space)
        0xA2,
        0x00, // LDX #0
        0x9D,
        0x00,
        0x04, // STA $0400,X
        0x9D,
        0x00,
        0x05, // STA $0500,X
        0x9D,
        0x00,
        0x06, // STA $0600,X
        0x9D,
        0x00,
        0x07, // STA $0700,X
        0xE8, // INX
        0xD0,
        0xF1, // BNE the stores
        0xBD,
        0x00,
        0x00, // LDA say,X (patched below)
        0xF0,
        0x06, // BEQ shown
        0x9D,
        static_cast<std::uint8_t>(row12 & 0xFF),
        static_cast<std::uint8_t>(row12 >> 8),
        0xE8, // INX
        0xD0,
        0xF5, // BNE the load
        0x8D,
        0x0C,
        0xC0, // shown: STA $C00C (40 columns)
        0x8D,
        0x00,
        0xC0, // STA $C000 (80STORE off)
        0x2C,
        0x51,
        0xC0, // BIT $C051 (text)
        0x2C,
        0x54,
        0xC0, // BIT $C054 (page 1)
        0x4C,
        0x00,
        0x00, // JMP here (patched below)
    };
    std::uint16_t here = static_cast<std::uint16_t>(0x0800 + c.size() - 3);
    c[c.size() - 2] = static_cast<std::uint8_t>(here & 0xFF);
    c[c.size() - 1] = static_cast<std::uint8_t>(here >> 8);
    std::uint16_t text = static_cast<std::uint16_t>(0x0800 + c.size());
    c[21] = static_cast<std::uint8_t>(text & 0xFF);
    c[22] = static_cast<std::uint8_t>(text >> 8);
    for (int i = 0; i < length; i++) {
        c.push_back(static_cast<std::uint8_t>(say[i] | 0x80));
    }
    c.push_back(0);
    return c;
}

} // namespace

void MakeDisk2(const std::vector<DiskTable>& tables, DiskSectors& disk) {
    std::vector<std::uint8_t> boot = Disk2BootSector();
    std::memcpy(disk.data[0][0], boot.data(), boot.size());
    WriteDiskTables(tables, disk, 2);
}

} // namespace tisple
