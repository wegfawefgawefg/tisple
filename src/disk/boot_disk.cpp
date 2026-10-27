// a self-booting disk: our loader on track 0, the program's segments from track 1 on, each
// from a track of its own and around track 17, and an empty DOS 3.3 volume around them
// (boot_loader.hpp)

#include "disk/boot_loader.hpp"
#include "disk/dos33.hpp"

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
    default:
        return LoaderBank::kMain;
    }
}

int NextTrack(int track) {
    track++;
    return track == kBootCatalogTrack ? track + 1 : track;
}

} // namespace

bool MakeBootDisk(const std::vector<MemorySegment>& segments, uint16_t entry, DiskSectors& disk,
                  BootDiskInfo& info, std::string& error) {
    if (segments.empty()) {
        error = "the program is empty";
        return false;
    }
    if (segments.size() > kMaxBootSegments) {
        error = "the program has " + std::to_string(segments.size()) +
                " segments; a boot disk holds " + std::to_string(kMaxBootSegments);
        return false;
    }
    LoaderPlan plan;
    plan.entry = entry;
    info = BootDiskInfo();
    for (const MemorySegment& segment : segments) {
        if (!CheckSegment(segment, error)) {
            return false;
        }
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
    info.first_page = plan.segments[0].first_page;
    info.segments = segments.size();
    return true;
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
