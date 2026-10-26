#pragma once

// `tisple machine --disk a.dsk [--disk2 b.dsk] [--disk-rom DISK2.rom] [--write-back]`: the
// disk ii card in slot 6 with its drives, set up from files, and written back after the run

#include "machine/apple2.hpp"

#include <string>
#include <vector>

namespace tisple {

constexpr int kDiskSlot = 6;

struct MachineDisks {
    std::string paths[2];           // drive 1 and 2 image files ("" for none)
    std::vector<uint8_t> images[2]; // or images already in memory (`tisple run --disk`)
    DiskFormat formats[2] = {DiskFormat::kNone, DiskFormat::kNone};
    std::string rom_path; // DISK2.rom; "" for our own boot rom
    bool write_back = false;
    bool any() const {
        return !paths[0].empty() || !paths[1].empty() || !images[0].empty() || !images[1].empty() ||
               !rom_path.empty();
    }
};

// the card and the disks; a disk with our boot loader on it sets the machine's return pc
bool SetUpDisks(Apple2& a, const MachineDisks& disks, std::string& error);
// the drives that were written, saved to their files; messages for each
bool WriteBackDisks(const Apple2& a, const MachineDisks& disks, std::string& report);
// a status line: "disk: drive 1 track 3, motor off"
std::string DescribeDisks(const Apple2& a);

} // namespace tisple
