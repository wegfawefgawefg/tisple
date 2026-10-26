#include "disk/dos33.hpp"

#include <cstring>

namespace tisple {

namespace {

constexpr int kVtocTrack = 17;
constexpr int kCatalogEntries = 7;
constexpr int kEntryBytes = 35;
constexpr int kFirstEntry = 0x0B;
constexpr int kNameBytes = 30;
constexpr int kPairsPerList = 122;
constexpr int kBitmapStart = 0x38;

uint8_t* Sector(DiskSectors& disk, int track, int logical) {
    return disk.data[track][PhysicalOfDosLogical(logical)];
}

const uint8_t* Sector(const DiskSectors& disk, int track, int logical) {
    return disk.data[track][PhysicalOfDosLogical(logical)];
}

uint8_t* Vtoc(DiskSectors& disk) {
    return Sector(disk, kVtocTrack, 0);
}

const uint8_t* Vtoc(const DiskSectors& disk) {
    return Sector(disk, kVtocTrack, 0);
}

// a track's free bits: byte 0 holds sectors 15-8, byte 1 sectors 7-0
bool IsFree(const DiskSectors& disk, int track, int sector) {
    const uint8_t* map = Vtoc(disk) + kBitmapStart + track * 4;
    int byte = sector >= 8 ? 0 : 1;
    return (map[byte] & (1 << (sector & 7))) != 0;
}

void SetFree(DiskSectors& disk, int track, int sector, bool free) {
    uint8_t* map = Vtoc(disk) + kBitmapStart + track * 4;
    int byte = sector >= 8 ? 0 : 1;
    uint8_t bit = static_cast<uint8_t>(1 << (sector & 7));
    map[byte] = static_cast<uint8_t>(free ? (map[byte] | bit) : (map[byte] & ~bit));
}

std::string Upper(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

std::string EntryName(const uint8_t* entry) {
    std::string name;
    for (int i = 0; i < kNameBytes; i++) {
        name += static_cast<char>(entry[3 + i] & 0x7F);
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    return name;
}

char TypeLetter(uint8_t type) {
    switch (type & 0x7F) {
    case 0x00:
        return 'T';
    case 0x01:
        return 'I';
    case 0x02:
        return 'A';
    case 0x04:
        return 'B';
    case 0x08:
        return 'S';
    case 0x10:
        return 'R';
    case 0x20:
        return 'a';
    default:
        return 'b';
    }
}

int TypeCode(char letter) {
    switch (letter) {
    case 'T':
        return 0x00;
    case 'I':
        return 0x01;
    case 'A':
        return 0x02;
    case 'B':
        return 0x04;
    case 'S':
        return 0x08;
    case 'R':
        return 0x10;
    default:
        return -1;
    }
}

struct EntryRef {
    int track = 0;
    int sector = 0;
    int index = 0; // entry within the sector
};

uint8_t* EntryAt(DiskSectors& disk, const EntryRef& ref) {
    return Sector(disk, ref.track, ref.sector) + kFirstEntry + ref.index * kEntryBytes;
}

const uint8_t* EntryAt(const DiskSectors& disk, const EntryRef& ref) {
    return Sector(disk, ref.track, ref.sector) + kFirstEntry + ref.index * kEntryBytes;
}

// every catalog entry slot, in catalog order; false on a broken chain
bool CatalogSlots(const DiskSectors& disk, std::vector<EntryRef>& slots, std::string& error) {
    const uint8_t* vtoc = Vtoc(disk);
    int track = vtoc[1];
    int sector = vtoc[2];
    int guard = 0;
    while (track != 0) {
        if (track >= kDiskTracks || sector >= kSectorsPerTrack || ++guard > 64) {
            error = "the catalog chain is broken (not a DOS 3.3 disk?)";
            return false;
        }
        for (int i = 0; i < kCatalogEntries; i++) {
            slots.push_back({track, sector, i});
        }
        const uint8_t* s = Sector(disk, track, sector);
        track = s[1];
        sector = s[2];
    }
    return true;
}

bool FindEntry(const DiskSectors& disk, const std::string& name, EntryRef& found,
               std::string& error) {
    std::vector<EntryRef> slots;
    if (!CatalogSlots(disk, slots, error)) {
        return false;
    }
    std::string want = Upper(name);
    for (const EntryRef& ref : slots) {
        const uint8_t* entry = EntryAt(disk, ref);
        if (entry[0] == 0) {
            break; // never used: the end of the catalog
        }
        if (entry[0] != 0xFF && EntryName(entry) == want) {
            found = ref;
            return true;
        }
    }
    error = "no file named '" + want + "'";
    return false;
}

// a free sector, from track 18 outward then 16 down, as DOS fills a disk
bool AllocateSector(DiskSectors& disk, int& track, int& sector) {
    int order[kDiskTracks];
    int n = 0;
    for (int t = kVtocTrack + 1; t < kDiskTracks; t++) {
        order[n++] = t;
    }
    for (int t = kVtocTrack - 1; t >= 1; t--) {
        order[n++] = t;
    }
    for (int i = 0; i < n; i++) {
        for (int s = kSectorsPerTrack - 1; s >= 0; s--) {
            if (IsFree(disk, order[i], s)) {
                SetFree(disk, order[i], s, false);
                track = order[i];
                sector = s;
                return true;
            }
        }
    }
    return false;
}

// the file's data sectors in order, following its track/sector lists
bool DataSectors(const DiskSectors& disk, const uint8_t* entry,
                 std::vector<std::pair<int, int>>& data, std::vector<std::pair<int, int>>& lists,
                 std::string& error) {
    int track = entry[0];
    int sector = entry[1];
    int guard = 0;
    while (track != 0) {
        if (track >= kDiskTracks || sector >= kSectorsPerTrack || ++guard > 1000) {
            error = "the file's track/sector list is broken";
            return false;
        }
        lists.push_back({track, sector});
        const uint8_t* list = Sector(disk, track, sector);
        for (int i = 0; i < kPairsPerList; i++) {
            int t = list[0x0C + 2 * i];
            int s = list[0x0D + 2 * i];
            if (t == 0) {
                continue; // a hole (random-access files) or the end
            }
            if (t >= kDiskTracks || s >= kSectorsPerTrack) {
                error = "the file's track/sector list is broken";
                return false;
            }
            data.push_back({t, s});
        }
        track = list[1];
        sector = list[2];
    }
    return true;
}

} // namespace

void FormatDos33(DiskSectors& disk, uint8_t volume) {
    std::memset(disk.data, 0, sizeof(disk.data));
    uint8_t* vtoc = Vtoc(disk);
    vtoc[0x01] = kVtocTrack; // first catalog sector
    vtoc[0x02] = 15;
    vtoc[0x03] = 3; // DOS release
    vtoc[0x06] = volume;
    vtoc[0x27] = kPairsPerList;
    vtoc[0x30] = kVtocTrack + 1; // last track allocated, and the direction
    vtoc[0x31] = 1;
    vtoc[0x34] = kDiskTracks;
    vtoc[0x35] = kSectorsPerTrack;
    vtoc[0x36] = 0x00; // 256 bytes a sector
    vtoc[0x37] = 0x01;
    for (int t = 1; t < kDiskTracks; t++) {
        if (t != kVtocTrack) {
            vtoc[kBitmapStart + t * 4] = 0xFF;
            vtoc[kBitmapStart + t * 4 + 1] = 0xFF;
        }
    }
    for (int s = 15; s >= 1; s--) {
        uint8_t* cat = Sector(disk, kVtocTrack, s);
        cat[1] = s > 1 ? kVtocTrack : 0;
        cat[2] = static_cast<uint8_t>(s > 1 ? s - 1 : 0);
    }
}

bool IsDos33(const DiskSectors& disk) {
    const uint8_t* vtoc = Vtoc(disk);
    return vtoc[0x01] > 0 && vtoc[0x01] < kDiskTracks && vtoc[0x02] < kSectorsPerTrack &&
           vtoc[0x27] == kPairsPerList && vtoc[0x34] == kDiskTracks &&
           vtoc[0x35] == kSectorsPerTrack;
}

void DosReserveTrack(DiskSectors& disk, int track) {
    for (int s = 0; s < kSectorsPerTrack; s++) {
        SetFree(disk, track, s, false);
    }
}

uint32_t DosFreeSectors(const DiskSectors& disk) {
    uint32_t count = 0;
    for (int t = 0; t < kDiskTracks; t++) {
        for (int s = 0; s < kSectorsPerTrack; s++) {
            count += IsFree(disk, t, s) ? 1 : 0;
        }
    }
    return count;
}

bool DosCatalog(const DiskSectors& disk, std::vector<DosFile>& files, std::string& error) {
    if (!IsDos33(disk)) {
        error = "not a DOS 3.3 disk (no VTOC at track 17 sector 0)";
        return false;
    }
    std::vector<EntryRef> slots;
    if (!CatalogSlots(disk, slots, error)) {
        return false;
    }
    for (const EntryRef& ref : slots) {
        const uint8_t* entry = EntryAt(disk, ref);
        if (entry[0] == 0) {
            break;
        }
        if (entry[0] == 0xFF) {
            continue;
        }
        DosFile file;
        file.name = EntryName(entry);
        file.type = TypeLetter(entry[2]);
        file.locked = (entry[2] & 0x80) != 0;
        file.sectors = static_cast<uint32_t>(entry[33] | (entry[34] << 8));
        files.push_back(file);
    }
    return true;
}

bool DosPutFile(DiskSectors& disk, const std::string& name, char type,
                const std::vector<uint8_t>& contents, std::string& error) {
    std::string upper = Upper(name);
    int code = TypeCode(type);
    if (code < 0) {
        error = std::string("unknown file type '") + type + "' (T I A B S R)";
        return false;
    }
    if (upper.empty() || upper.size() > kNameBytes) {
        error = "a DOS 3.3 name is 1 to 30 characters";
        return false;
    }
    if (!IsDos33(disk)) {
        error = "not a DOS 3.3 disk (no VTOC at track 17 sector 0)";
        return false;
    }
    EntryRef existing;
    std::string ignored;
    if (FindEntry(disk, upper, existing, ignored)) {
        error = "'" + upper + "' is already on the disk; delete it first";
        return false;
    }
    std::vector<EntryRef> slots;
    if (!CatalogSlots(disk, slots, error)) {
        return false;
    }
    EntryRef slot;
    bool have_slot = false;
    for (const EntryRef& ref : slots) {
        uint8_t first = EntryAt(disk, ref)[0];
        if (first == 0 || first == 0xFF) {
            slot = ref;
            have_slot = true;
            break;
        }
    }
    if (!have_slot) {
        error = "the catalog is full";
        return false;
    }
    size_t data_count = (contents.size() + kSectorBytes - 1) / kSectorBytes;
    data_count = data_count == 0 ? 1 : data_count;
    size_t list_count = (data_count + kPairsPerList - 1) / kPairsPerList;
    if (data_count + list_count > DosFreeSectors(disk)) {
        error = "the disk is full: the file needs " + std::to_string(data_count + list_count) +
                " sectors, " + std::to_string(DosFreeSectors(disk)) + " are free";
        return false;
    }
    // lists first, each followed by its data, like DOS
    int prev_track = -1;
    int prev_sector = -1;
    int first_track = 0;
    int first_sector = 0;
    size_t written = 0;
    for (size_t l = 0; l < list_count; l++) {
        int lt = 0;
        int ls = 0;
        AllocateSector(disk, lt, ls);
        uint8_t* list = Sector(disk, lt, ls);
        std::memset(list, 0, kSectorBytes);
        size_t offset = l * kPairsPerList;
        list[0x05] = static_cast<uint8_t>(offset & 0xFF);
        list[0x06] = static_cast<uint8_t>(offset >> 8);
        if (prev_track < 0) {
            first_track = lt;
            first_sector = ls;
        } else {
            uint8_t* prev = Sector(disk, prev_track, prev_sector);
            prev[1] = static_cast<uint8_t>(lt);
            prev[2] = static_cast<uint8_t>(ls);
        }
        for (int i = 0; i < kPairsPerList && written < data_count; i++) {
            int dt = 0;
            int ds = 0;
            AllocateSector(disk, dt, ds);
            uint8_t* data = Sector(disk, dt, ds);
            std::memset(data, 0, kSectorBytes);
            size_t start = written * kSectorBytes;
            size_t n = contents.size() > start ? contents.size() - start : 0;
            n = n > kSectorBytes ? kSectorBytes : n;
            if (n > 0) {
                std::memcpy(data, contents.data() + start, n);
            }
            list[0x0C + 2 * i] = static_cast<uint8_t>(dt);
            list[0x0D + 2 * i] = static_cast<uint8_t>(ds);
            written++;
        }
        prev_track = lt;
        prev_sector = ls;
    }
    uint8_t* entry = EntryAt(disk, slot);
    entry[0] = static_cast<uint8_t>(first_track);
    entry[1] = static_cast<uint8_t>(first_sector);
    entry[2] = static_cast<uint8_t>(code);
    for (int i = 0; i < kNameBytes; i++) {
        char c = i < static_cast<int>(upper.size()) ? upper[static_cast<size_t>(i)] : ' ';
        entry[3 + i] = static_cast<uint8_t>(c | 0x80);
    }
    size_t total = data_count + list_count;
    entry[33] = static_cast<uint8_t>(total & 0xFF);
    entry[34] = static_cast<uint8_t>(total >> 8);
    return true;
}

bool DosGetFile(const DiskSectors& disk, const std::string& name, DosFile& file,
                std::vector<uint8_t>& contents, std::string& error) {
    EntryRef ref;
    if (!IsDos33(disk)) {
        error = "not a DOS 3.3 disk (no VTOC at track 17 sector 0)";
        return false;
    }
    if (!FindEntry(disk, name, ref, error)) {
        return false;
    }
    const uint8_t* entry = EntryAt(disk, ref);
    file.name = EntryName(entry);
    file.type = TypeLetter(entry[2]);
    file.locked = (entry[2] & 0x80) != 0;
    file.sectors = static_cast<uint32_t>(entry[33] | (entry[34] << 8));
    std::vector<std::pair<int, int>> data;
    std::vector<std::pair<int, int>> lists;
    if (!DataSectors(disk, entry, data, lists, error)) {
        return false;
    }
    contents.clear();
    for (const auto& ts : data) {
        const uint8_t* s = Sector(disk, ts.first, ts.second);
        contents.insert(contents.end(), s, s + kSectorBytes);
    }
    return true;
}

bool DosDeleteFile(DiskSectors& disk, const std::string& name, std::string& error) {
    EntryRef ref;
    if (!IsDos33(disk)) {
        error = "not a DOS 3.3 disk (no VTOC at track 17 sector 0)";
        return false;
    }
    if (!FindEntry(disk, name, ref, error)) {
        return false;
    }
    uint8_t* entry = EntryAt(disk, ref);
    if ((entry[2] & 0x80) != 0) {
        error = "'" + Upper(name) + "' is locked";
        return false;
    }
    std::vector<std::pair<int, int>> data;
    std::vector<std::pair<int, int>> lists;
    if (!DataSectors(disk, entry, data, lists, error)) {
        return false;
    }
    for (const auto& ts : data) {
        SetFree(disk, ts.first, ts.second, true);
    }
    for (const auto& ts : lists) {
        SetFree(disk, ts.first, ts.second, true);
    }
    entry[3 + kNameBytes - 1] = entry[0]; // DOS keeps the old track in the name's last byte
    entry[0] = 0xFF;
    return true;
}

std::vector<uint8_t> DosBinaryContents(const std::vector<uint8_t>& data, uint16_t address) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(address & 0xFF));
    out.push_back(static_cast<uint8_t>(address >> 8));
    out.push_back(static_cast<uint8_t>(data.size() & 0xFF));
    out.push_back(static_cast<uint8_t>((data.size() >> 8) & 0xFF));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

bool DosBinaryData(const std::vector<uint8_t>& contents, uint16_t& address,
                   std::vector<uint8_t>& data) {
    if (contents.size() < 4) {
        return false;
    }
    address = static_cast<uint16_t>(contents[0] | (contents[1] << 8));
    size_t length = static_cast<size_t>(contents[2] | (contents[3] << 8));
    if (length + 4 > contents.size()) {
        return false;
    }
    data.assign(contents.begin() + 4, contents.begin() + 4 + static_cast<long>(length));
    return true;
}

std::vector<uint8_t> DosTextContents(const std::string& text) {
    std::vector<uint8_t> out;
    for (char c : text) {
        if (c == '\r') {
            continue;
        }
        uint8_t b = c == '\n' ? 0x0D : static_cast<uint8_t>(c & 0x7F);
        out.push_back(static_cast<uint8_t>(b | 0x80));
    }
    return out;
}

std::string DosTextData(const std::vector<uint8_t>& contents) {
    std::string out;
    for (uint8_t b : contents) {
        if (b == 0) {
            break;
        }
        char c = static_cast<char>(b & 0x7F);
        out += c == '\r' ? '\n' : c;
    }
    return out;
}

} // namespace tisple
