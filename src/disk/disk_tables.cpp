// disk tables' places on the disk, their records, and their bytes on the disk (disk_tables.hpp)

#include "disk/disk_tables.hpp"

#include "disk/boot_loader.hpp"

#include <cstring>

namespace tisple {

namespace {

int NextTableTrack(int track) {
    track++;
    return track == kBootCatalogTrack ? track + 1 : track;
}

// every table laid out from track start on; false when they go past the last track
bool LayFrom(std::vector<DiskTable>& tables, int start) {
    int track = start;
    bool used[kSectorsPerTrack] = {};
    int filled = 0;
    int last = -1; // the physical sector of the piece before, on this track
    for (DiskTable& table : tables) {
        table.tracks.clear();
        table.sectors.clear();
        std::uint32_t pieces = DiskTableSectors(static_cast<std::uint32_t>(table.bytes.size()));
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
            int p = last < 0 ? 0 : (last + table.interleave) % kSectorsPerTrack;
            while (used[p]) {
                p = (p + 1) % kSectorsPerTrack;
            }
            used[p] = true;
            filled++;
            last = p;
            table.tracks.push_back(static_cast<std::uint8_t>(track));
            table.sectors.push_back(static_cast<std::uint8_t>(p));
        }
    }
    return true;
}

} // namespace

bool PlaceDiskTables(std::vector<DiskTable>& tables, std::string& error) {
    std::uint32_t pieces = 0;
    for (const DiskTable& table : tables) {
        if (table.bytes.empty() || table.bytes.size() > kMaxDiskTableBytes) {
            error = "a table :on disk holds 1 to " + std::to_string(kMaxDiskTableBytes) +
                    " bytes, not " + std::to_string(table.bytes.size());
            return false;
        }
        pieces += DiskTableSectors(static_cast<std::uint32_t>(table.bytes.size()));
    }
    for (int start = kBootLastTrack; start >= kBootFirstTrack; start--) {
        if (start != kBootCatalogTrack && LayFrom(tables, start)) {
            return true;
        }
    }
    for (DiskTable& table : tables) {
        table.tracks.clear();
        table.sectors.clear();
    }
    error = "the tables :on disk are " + std::to_string(pieces) +
            " sectors; a boot disk holds 528 besides the loader (tracks 1-34 but 17)";
    return false;
}

int DiskTablesFirstTrack(const std::vector<DiskTable>& tables) {
    for (const DiskTable& table : tables) {
        if (!table.tracks.empty()) {
            return table.tracks[0];
        }
    }
    return 0;
}

std::vector<std::uint8_t> DiskTableRecord(const DiskTable& table) {
    std::uint32_t size = static_cast<std::uint32_t>(table.bytes.size());
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(size & 0xFF));
    out.push_back(static_cast<std::uint8_t>(size >> 8));
    int first = table.tracks.empty() ? 0 : table.tracks[0];
    out.push_back(static_cast<std::uint8_t>(first));
    for (std::size_t k = 0; k < table.tracks.size(); k++) {
        int offset = table.tracks[k] - first;
        out.push_back(static_cast<std::uint8_t>((offset << 4) | table.sectors[k]));
    }
    return out;
}

void WriteDiskTables(const std::vector<DiskTable>& tables, DiskSectors& disk) {
    for (const DiskTable& table : tables) {
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
