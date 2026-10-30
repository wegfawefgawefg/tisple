// a self-booting disk: our loader on track 0, the program's segments from track 1 on, each
// from a track of its own and around track 17, and an empty DOS 3.3 volume around them
// (boot_loader.hpp)

#include "disk/boot_loader.hpp"
#include "disk/disk_reader.hpp"
#include "disk/disk_tables.hpp"
#include "disk/dos33.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tisple {

namespace {

std::string SegmentRange(const MemorySegment& segment) {
    char text[64];
    uint32_t end = segment.address + static_cast<uint32_t>(segment.bytes.size());
    std::snprintf(text, sizeof(text), "$%04X-$%04X", segment.address, end - 1);
    return text;
}

// where each bank's segments must load
bool CheckSegment(const MemorySegment& segment, std::string& error) {
    uint32_t start = segment.address;
    uint32_t end = start + static_cast<uint32_t>(segment.bytes.size());
    if (segment.bytes.empty()) {
        error = "the program is empty";
        return false;
    }
    char text[200];
    if (segment.bank == MemoryBank::kMain && (start < kBootLoadLow || end > kBootLoadHigh)) {
        std::snprintf(text, sizeof(text),
                      "a self-booting program must load in $%04X-$%04X; this one is %s",
                      kBootLoadLow, kBootLoadHigh - 1, SegmentRange(segment).c_str());
        error = text;
        return false;
    }
    if (segment.bank == MemoryBank::kCard && (start < 0xD000 || end > 0x10000)) {
        error = "a language card segment loads in $D000-$FFFF, not " + SegmentRange(segment);
        return false;
    }
    if (segment.bank == MemoryBank::kCardBank1 && (start < 0xD000 || end > 0xE000)) {
        error = "a language card bank 1 segment loads in $D000-$DFFF, not " + SegmentRange(segment);
        return false;
    }
    if (segment.bank == MemoryBank::kAux && (start < 0x0200 || end > 0xC000)) {
        error = "an aux segment loads in $0200-$BFFF, not " + SegmentRange(segment);
        return false;
    }
    return true;
}

LoaderBank BankFor(MemoryBank bank) {
    switch (bank) {
    case MemoryBank::kCard:
        return LoaderBank::kCard;
    case MemoryBank::kAux:
        return LoaderBank::kAux;
    case MemoryBank::kCardBank1:
        return LoaderBank::kCardBank1;
    default:
        return LoaderBank::kMain;
    }
}

MemorySegment Merged(const MemorySegment& a, const MemorySegment& b) {
    uint32_t a_end = a.address + static_cast<uint32_t>(a.bytes.size());
    uint32_t b_end = b.address + static_cast<uint32_t>(b.bytes.size());
    MemorySegment out;
    out.bank = a.bank;
    out.address = std::min(a.address, b.address);
    out.bytes.assign(std::max(a_end, b_end) - out.address, 0);
    std::memcpy(out.bytes.data() + (a.address - out.address), a.bytes.data(), a.bytes.size());
    std::memcpy(out.bytes.data() + (b.address - out.address), b.bytes.data(), b.bytes.size());
    return out;
}

bool BytesOverlap(const MemorySegment& a, const MemorySegment& b) {
    uint32_t a_end = a.address + static_cast<uint32_t>(a.bytes.size());
    uint32_t b_end = b.address + static_cast<uint32_t>(b.bytes.size());
    return a.bank == b.bank && a.address < b_end && b.address < a_end;
}

bool SharePage(const MemorySegment& a, const MemorySegment& b) {
    uint32_t a_last = (a.address + static_cast<uint32_t>(a.bytes.size()) - 1) >> 8;
    uint32_t b_last = (b.address + static_cast<uint32_t>(b.bytes.size()) - 1) >> 8;
    return a.bank == b.bank && (a.address >> 8) <= b_last && (b.address >> 8) <= a_last;
}

// the segments as the disk holds them: the loader writes whole pages, so segments of one
// memory that share a page become one, with zeros between them (else each would clear the
// others' bytes there); the aux ones first, so that a main page can hold their sectors
// before the copy
std::vector<MemorySegment> DiskSegments(const std::vector<MemorySegment>& segments) {
    std::vector<MemorySegment> merged;
    for (const MemorySegment& segment : segments) {
        MemorySegment s = segment;
        for (size_t i = 0; i < merged.size();) {
            if (!SharePage(merged[i], s)) {
                i++;
                continue;
            }
            s = Merged(merged[i], s);
            merged.erase(merged.begin() + static_cast<std::ptrdiff_t>(i));
            i = 0;
        }
        merged.push_back(s);
    }
    std::vector<MemorySegment> out;
    for (const MemorySegment& s : merged) {
        if (s.bank == MemoryBank::kAux) {
            out.push_back(s);
        }
    }
    for (const MemorySegment& s : merged) {
        if (s.bank != MemoryBank::kAux) {
            out.push_back(s);
        }
    }
    return out;
}

// the tables :on disk on their tracks, which the program's must stay below
bool PutDiskTables(const BootExtras& extras, DiskSectors& disk, BootDiskInfo& info,
                   std::string& error) {
    if (extras.tables == nullptr || extras.tables->empty()) {
        return true;
    }
    int first = DiskTablesFirstTrack(*extras.tables);
    if (first <= info.last_track) {
        error = "the program takes " + BootTracksText(info) + " and its tables :on disk tracks " +
                std::to_string(first) + "-" + std::to_string(kBootLastTrack) +
                "; together they do not fit the disk";
        return false;
    }
    WriteDiskTables(*extras.tables, disk);
    for (const DiskTable& table : *extras.tables) {
        for (uint8_t track : table.tracks) {
            DosReserveTrack(disk, track);
        }
    }
    info.table_track = first;
    return true;
}

int NextTrack(int track) {
    track++;
    return track == kBootCatalogTrack ? track + 1 : track;
}

} // namespace

bool MakeBootDisk(const std::vector<MemorySegment>& segments_in, uint16_t entry, DiskSectors& disk,
                  BootDiskInfo& info, std::string& error, const BootExtras& extras) {
    if (segments_in.empty()) {
        error = "the program is empty";
        return false;
    }
    std::vector<MemorySegment> program = segments_in;
    if (extras.reader != 0) {
        MemorySegment reader;
        reader.bank = MemoryBank::kMain;
        reader.address = extras.reader;
        if (!BuildDiskReader(extras.reader, reader.bytes, error)) {
            return false;
        }
        if (extras.work + kReaderWork != extras.reader || extras.work_room < kReaderWork) {
            error = "lib/disk's disk.work must be the 768 bytes just below disk.reader";
            return false;
        }
        if (reader.bytes.size() > extras.reader_room) {
            error = "lib/disk's reader is " + std::to_string(reader.bytes.size()) +
                    " bytes, more than the " + std::to_string(extras.reader_room) +
                    " disk.reader keeps for it";
            return false;
        }
        program.push_back(reader);
    }
    for (size_t i = 0; i < program.size(); i++) {
        if (!CheckSegment(program[i], error)) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (BytesOverlap(program[i], program[j])) {
                error = "the segments at " + SegmentRange(program[j]) + " and " +
                        SegmentRange(program[i]) + " in " + MemoryBankName(program[i].bank) +
                        " memory overlap";
                return false;
            }
        }
    }
    std::vector<MemorySegment> segments = DiskSegments(program);
    if (segments.size() > kMaxBootSegments) {
        error = "the program has " + std::to_string(segments.size()) +
                " segments; a boot disk holds " + std::to_string(kMaxBootSegments);
        return false;
    }
    LoaderPlan plan;
    plan.entry = entry;
    plan.slot_at = extras.reader != 0 ? static_cast<uint16_t>(extras.reader + kReaderSlot) : 0;
    info = BootDiskInfo();
    for (const MemorySegment& segment : segments) {
        if (segment.bank == MemoryBank::kMain) {
            plan.bounce_page = static_cast<uint8_t>(segment.address >> 8);
            break;
        }
    }
    for (const MemorySegment& segment : segments) {
        uint32_t start = segment.address & 0xFF00u;
        uint32_t end = segment.address + static_cast<uint32_t>(segment.bytes.size());
        LoaderSegment piece;
        piece.first_page = static_cast<uint8_t>(start >> 8);
        piece.pages = static_cast<uint8_t>((end - start + 0xFF) / 0x100);
        piece.bank = BankFor(segment.bank);
        plan.segments.push_back(piece);
        info.pages += piece.pages;
    }
    uint8_t loader[kBootLoaderSectors][kSectorBytes];
    if (!BuildLoader(plan, loader, info.return_pc, error)) {
        return false;
    }
    FormatDos33(disk, kDefaultVolume);
    for (int s = 0; s < kBootLoaderSectors; s++) {
        std::memcpy(disk.data[0][s], loader[s], kSectorBytes);
    }
    DosReserveTrack(disk, 0);
    int track = kBootFirstTrack;
    for (size_t s = 0; s < segments.size(); s++) {
        const MemorySegment& segment = segments[s];
        const LoaderSegment& piece = plan.segments[s];
        std::vector<uint8_t> pages(piece.pages * 0x100u, 0);
        std::memcpy(pages.data() + (segment.address & 0xFF), segment.bytes.data(),
                    segment.bytes.size());
        for (uint32_t k = 0; k < piece.pages; k++) {
            int physical = static_cast<int>(k % kSectorsPerTrack);
            if (k > 0 && physical == 0) {
                track = NextTrack(track);
            }
            if (track > kBootLastTrack) {
                error = "the program is " + std::to_string(info.pages) +
                        " pages; a boot disk holds 528 (tracks 1-34 but 17)";
                return false;
            }
            std::memcpy(disk.data[track][physical], pages.data() + k * 0x100, 0x100);
            DosReserveTrack(disk, track);
        }
        info.last_track = track;
        track = NextTrack(track);
    }
    info.first_page = static_cast<uint16_t>(program[0].address >> 8);
    info.segments = segments.size();
    return PutDiskTables(extras, disk, info, error);
}

bool MakeBootDisk(const std::vector<uint8_t>& program, uint16_t origin, DiskSectors& disk,
                  BootDiskInfo& info, std::string& error) {
    MemorySegment main;
    main.bank = MemoryBank::kMain;
    main.address = origin;
    main.bytes = program;
    return MakeBootDisk(std::vector<MemorySegment>{main}, origin, disk, info, error);
}

std::string BootTracksText(const BootDiskInfo& info) {
    if (info.last_track == kBootFirstTrack) {
        return "track " + std::to_string(kBootFirstTrack);
    }
    return "tracks " + std::to_string(kBootFirstTrack) + "-" + std::to_string(info.last_track);
}

} // namespace tisple
