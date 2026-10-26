#pragma once

// the DOS 3.3 file system on a 140 KB disk, after Beneath Apple DOS chapter 4:
// - the VTOC at track 17 sector 0: the catalog's first sector, the volume number, and a
//   bitmap of free sectors, 4 bytes a track from $38 (bit set: free).
// - the catalog: a chain of sectors from track 17 sector 15 down, 7 entries of 35 bytes each
//   from $0B: the track/sector list's track and sector (track $FF: deleted, the old track
//   kept in the name's last byte, 0: never used), the type (bit 7: locked), 30 name bytes
//   (high bit set, space padded) and the length in sectors.
// - a track/sector list: next list at $01, then up to 122 data sector pairs from $0C.
// - file contents: B (binary) starts with its load address and length, A (Applesoft) with
//   its length, T (text) is high-bit ASCII ended by a zero.
// sector numbers here are DOS's logical ones; DiskSectors holds physical sectors and the
// skew is applied on every access.

#include "disk/nibble.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

struct DosFile {
    std::string name;
    char type = 'B'; // T I A B S R
    bool locked = false;
    uint32_t sectors = 0; // as the catalog counts them: the lists and the data
};

// an empty volume: VTOC and catalog; track 0 is marked in use (where a boot sector goes)
void FormatDos33(DiskSectors& disk, uint8_t volume);
bool IsDos33(const DiskSectors& disk);
// mark a track's sectors in use in the VTOC (a self-booting disk's program, say)
void DosReserveTrack(DiskSectors& disk, int track);
uint32_t DosFreeSectors(const DiskSectors& disk);

bool DosCatalog(const DiskSectors& disk, std::vector<DosFile>& files, std::string& error);
// contents as DOS stores them: for B the 4-byte address/length header comes first
bool DosPutFile(DiskSectors& disk, const std::string& name, char type,
                const std::vector<uint8_t>& contents, std::string& error);
// the stored contents, every data sector of the file in order
bool DosGetFile(const DiskSectors& disk, const std::string& name, DosFile& file,
                std::vector<uint8_t>& contents, std::string& error);
bool DosDeleteFile(DiskSectors& disk, const std::string& name, std::string& error);

// the file's own bytes from its stored contents, and back
std::vector<uint8_t> DosBinaryContents(const std::vector<uint8_t>& data, uint16_t address);
bool DosBinaryData(const std::vector<uint8_t>& contents, uint16_t& address,
                   std::vector<uint8_t>& data);
std::vector<uint8_t> DosTextContents(const std::string& text); // newlines become returns
std::string DosTextData(const std::vector<uint8_t>& contents);

} // namespace tisple
