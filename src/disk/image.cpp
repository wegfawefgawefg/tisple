#include "disk/image.hpp"

#include "machine/disk2.hpp"

#include <cstdio>
#include <cstring>
#include <memory>

namespace tisple {

namespace {

std::string Lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

bool EndsWith(const std::string& text, const char* suffix) {
    size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

SectorOrder OrderOf(DiskFormat format) {
    return format == DiskFormat::kProdos ? SectorOrder::kProdos : SectorOrder::kDos;
}

} // namespace

DiskFormat DiskFormatForPath(const std::string& path) {
    std::string name = Lower(path);
    if (EndsWith(name, ".dsk") || EndsWith(name, ".do")) {
        return DiskFormat::kDos;
    }
    if (EndsWith(name, ".po")) {
        return DiskFormat::kProdos;
    }
    if (EndsWith(name, ".nib")) {
        return DiskFormat::kNib;
    }
    return DiskFormat::kNone;
}

const char* DiskFormatName(DiskFormat format) {
    switch (format) {
    case DiskFormat::kDos:
        return "DOS 3.3 order";
    case DiskFormat::kProdos:
        return "ProDOS order";
    case DiskFormat::kNib:
        return "nibbles";
    default:
        return "none";
    }
}

bool SectorsFromImage(const std::vector<uint8_t>& file, SectorOrder order, DiskSectors& out,
                      std::string& error) {
    if (file.size() != kSectorImageBytes) {
        error = "a 140 KB disk image is " + std::to_string(kSectorImageBytes) + " bytes, not " +
                std::to_string(file.size());
        return false;
    }
    for (int t = 0; t < kDiskTracks; t++) {
        for (int p = 0; p < kSectorsPerTrack; p++) {
            size_t at = (static_cast<size_t>(t) * kSectorsPerTrack +
                         static_cast<size_t>(FileSectorOfPhysical(order, p))) *
                        kSectorBytes;
            std::memcpy(out.data[t][p], file.data() + at, kSectorBytes);
        }
    }
    return true;
}

std::vector<uint8_t> ImageFromSectors(const DiskSectors& sectors, SectorOrder order) {
    std::vector<uint8_t> file(kSectorImageBytes);
    for (int t = 0; t < kDiskTracks; t++) {
        for (int p = 0; p < kSectorsPerTrack; p++) {
            size_t at = (static_cast<size_t>(t) * kSectorsPerTrack +
                         static_cast<size_t>(FileSectorOfPhysical(order, p))) *
                        kSectorBytes;
            std::memcpy(file.data() + at, sectors.data[t][p], kSectorBytes);
        }
    }
    return file;
}

bool TracksFromImage(const std::vector<uint8_t>& file, DiskFormat format,
                     DiskTrack (&tracks)[kDiskTracks], std::string& error) {
    if (format == DiskFormat::kNib) {
        if (file.size() != kNibImageBytes) {
            error = "a .nib image is " + std::to_string(kNibImageBytes) + " bytes, not " +
                    std::to_string(file.size());
            return false;
        }
        for (int t = 0; t < kDiskTracks; t++) {
            TrackFromNibbles(file.data() + static_cast<size_t>(t) * kNibTrackBytes, kNibTrackBytes,
                             tracks[t]);
        }
        return true;
    }
    if (format != DiskFormat::kDos && format != DiskFormat::kProdos) {
        error = "unknown disk image format";
        return false;
    }
    std::unique_ptr<DiskSectors> sectors = std::make_unique<DiskSectors>();
    if (!SectorsFromImage(file, OrderOf(format), *sectors, error)) {
        return false;
    }
    for (int t = 0; t < kDiskTracks; t++) {
        NibblizeTrack(sectors->data[t], t, kDefaultVolume, tracks[t]);
    }
    return true;
}

bool ImageFromTracks(const DiskTrack (&tracks)[kDiskTracks], DiskFormat format,
                     std::vector<uint8_t>& file, std::string& error) {
    if (format == DiskFormat::kNib) {
        file.assign(kNibImageBytes, 0xFF);
        for (int t = 0; t < kDiskTracks; t++) {
            TrackNibbles(tracks[t], file.data() + static_cast<size_t>(t) * kNibTrackBytes,
                         kNibTrackBytes);
        }
        return true;
    }
    std::unique_ptr<DiskSectors> sectors = std::make_unique<DiskSectors>();
    for (int t = 0; t < kDiskTracks; t++) {
        bool found[kSectorsPerTrack];
        DenibblizeTrack(tracks[t], t, sectors->data[t], found);
        for (int p = 0; p < kSectorsPerTrack; p++) {
            if (!found[p]) {
                char text[96];
                std::snprintf(text, sizeof(text),
                              "track %d sector %d no longer reads as a 16-sector sector", t, p);
                error = text;
                return false;
            }
        }
    }
    file = ImageFromSectors(*sectors, OrderOf(format));
    return true;
}

bool InsertDiskImage(Apple2& a, int drive, const std::vector<uint8_t>& file, DiskFormat format,
                     bool write_protected, std::string& error) {
    DiskDrive& d = a.slots.disk2.drives[drive & 1];
    if (!TracksFromImage(file, format, d.tracks, error)) {
        return false;
    }
    InsertDisk(a, drive, format, write_protected);
    return true;
}

bool SaveDiskImage(const Apple2& a, int drive, std::vector<uint8_t>& file, std::string& error) {
    const DiskDrive& d = a.slots.disk2.drives[drive & 1];
    if (d.format == DiskFormat::kNone) {
        error = "no disk in the drive";
        return false;
    }
    return ImageFromTracks(d.tracks, d.format, file, error);
}

namespace {

struct TrackSet {
    DiskTrack t[kDiskTracks];
};

} // namespace

bool ReadDiskSectorsFile(const std::string& path, DiskSectors& sectors, std::string& error) {
    DiskFormat format = DiskFormatForPath(path);
    if (format == DiskFormat::kNone) {
        error = "'" + path + "' is not a disk image name (.dsk .do .po .nib)";
        return false;
    }
    std::vector<uint8_t> file;
    if (!ReadWholeFile(path, file)) {
        error = "cannot read '" + path + "'";
        return false;
    }
    if (format != DiskFormat::kNib) {
        return SectorsFromImage(file, OrderOf(format), sectors, error);
    }
    // a .nib through its tracks: it must read as 16 good sectors a track
    std::unique_ptr<TrackSet> tracks = std::make_unique<TrackSet>();
    std::vector<uint8_t> dos;
    if (!TracksFromImage(file, format, tracks->t, error) ||
        !ImageFromTracks(tracks->t, DiskFormat::kDos, dos, error)) {
        return false;
    }
    return SectorsFromImage(dos, SectorOrder::kDos, sectors, error);
}

bool WriteDiskSectorsFile(const std::string& path, const DiskSectors& sectors, std::string& error) {
    DiskFormat format = DiskFormatForPath(path);
    std::vector<uint8_t> file;
    if (format == DiskFormat::kNib) {
        std::unique_ptr<TrackSet> tracks = std::make_unique<TrackSet>();
        std::vector<uint8_t> dos = ImageFromSectors(sectors, SectorOrder::kDos);
        if (!TracksFromImage(dos, DiskFormat::kDos, tracks->t, error) ||
            !ImageFromTracks(tracks->t, DiskFormat::kNib, file, error)) {
            return false;
        }
    } else if (format == DiskFormat::kDos || format == DiskFormat::kProdos) {
        file = ImageFromSectors(sectors, OrderOf(format));
    } else {
        error = "'" + path + "' is not a disk image name (.dsk .do .po .nib)";
        return false;
    }
    if (!WriteWholeFile(path, file)) {
        error = "cannot write '" + path + "'";
        return false;
    }
    return true;
}

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& bytes) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    bytes.clear();
    uint8_t chunk[4096];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + got);
    }
    std::fclose(file);
    return true;
}

bool WriteWholeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && ok;
}

} // namespace tisple
