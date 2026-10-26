#pragma once

// disk image files: .dsk / .do (DOS 3.3 order) and .po (ProDOS order), 140 KB of sectors,
// and .nib, 35 tracks of 6656 raw disk bytes. to and from the sectors a disk holds, and the
// bit tracks a drive spins.

#include "disk/nibble.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tisple {

constexpr size_t kSectorImageBytes = kDiskTracks * kSectorsPerTrack * kSectorBytes; // 143360
constexpr size_t kNibTrackBytes = 6656;
constexpr size_t kNibImageBytes = kDiskTracks * kNibTrackBytes; // 232960

// by the file name's extension; kNone if it is none of ours
DiskFormat DiskFormatForPath(const std::string& path);
const char* DiskFormatName(DiskFormat format);

// sector images
bool SectorsFromImage(const std::vector<uint8_t>& file, SectorOrder order, DiskSectors& out,
                      std::string& error);
std::vector<uint8_t> ImageFromSectors(const DiskSectors& sectors, SectorOrder order);

// any format to tracks, and back. writing a sector image fails, naming the sector, when a
// track no longer reads as 16 good sectors (a program wrote something else there)
bool TracksFromImage(const std::vector<uint8_t>& file, DiskFormat format,
                     DiskTrack (&tracks)[kDiskTracks], std::string& error);
bool ImageFromTracks(const DiskTrack (&tracks)[kDiskTracks], DiskFormat format,
                     std::vector<uint8_t>& file, std::string& error);

// the drive glue
bool InsertDiskImage(Apple2& a, int drive, const std::vector<uint8_t>& file, DiskFormat format,
                     bool write_protected, std::string& error);
bool SaveDiskImage(const Apple2& a, int drive, std::vector<uint8_t>& file, std::string& error);

// image files as sectors, in the format the name says (a .nib must hold 16-sector tracks)
bool ReadDiskSectorsFile(const std::string& path, DiskSectors& sectors, std::string& error);
bool WriteDiskSectorsFile(const std::string& path, const DiskSectors& sectors, std::string& error);

// whole files
bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& bytes);
bool WriteWholeFile(const std::string& path, const std::vector<uint8_t>& bytes);

} // namespace tisple
