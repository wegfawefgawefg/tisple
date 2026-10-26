#include "disk/machine_disks.hpp"

#include "disk/boot_loader.hpp"
#include "disk/image.hpp"
#include "machine/disk2.hpp"

namespace tisple {

bool SetUpDisks(Apple2& a, const MachineDisks& disks, std::string& error) {
    std::vector<uint8_t> rom;
    if (!disks.rom_path.empty()) {
        if (!ReadWholeFile(disks.rom_path, rom)) {
            error = "cannot read '" + disks.rom_path + "'";
            return false;
        }
        if (rom.size() != 256) {
            error = "a disk ii boot rom is 256 bytes, not " + std::to_string(rom.size());
            return false;
        }
    }
    InsertDisk2Card(a, kDiskSlot, rom.empty() ? nullptr : rom.data());
    for (int d = 0; d < 2; d++) {
        std::vector<uint8_t> file = disks.images[d];
        DiskFormat format = disks.formats[d];
        if (!disks.paths[d].empty()) {
            format = DiskFormatForPath(disks.paths[d]);
            if (format == DiskFormat::kNone) {
                error = "'" + disks.paths[d] + "': not a disk image name (.dsk .do .po .nib)";
                return false;
            }
            if (!ReadWholeFile(disks.paths[d], file)) {
                error = "cannot read '" + disks.paths[d] + "'";
                return false;
            }
        }
        if (file.empty()) {
            continue;
        }
        if (!InsertDiskImage(a, d, file, format, false, error)) {
            error = (disks.paths[d].empty() ? std::string("disk") : disks.paths[d]) + ": " + error;
            return false;
        }
    }
    uint16_t return_pc = 0;
    if (HasDisk(a, 0) && FindBootLoader(a.slots.disk2.drives[0].tracks[0], return_pc)) {
        a.return_pc = return_pc;
    }
    return true;
}

bool WriteBackDisks(const Apple2& a, const MachineDisks& disks, std::string& report) {
    bool ok = true;
    for (int d = 0; d < 2; d++) {
        const DiskDrive& drive = a.slots.disk2.drives[d];
        if (drive.format == DiskFormat::kNone || !drive.dirty) {
            continue;
        }
        std::string name = "drive " + std::to_string(d + 1);
        if (disks.paths[d].empty()) {
            report += name + " was written; no file to save it to\n";
            continue;
        }
        std::vector<uint8_t> file;
        std::string error;
        if (!SaveDiskImage(a, d, file, error)) {
            report += disks.paths[d] + ": not saved: " + error + "\n";
            ok = false;
            continue;
        }
        if (!WriteWholeFile(disks.paths[d], file)) {
            report += disks.paths[d] + ": cannot write\n";
            ok = false;
            continue;
        }
        report += "wrote " + name + " back to " + disks.paths[d] + "\n";
    }
    return ok;
}

std::string DescribeDisks(const Apple2& a) {
    if (a.slots.kind[a.slots.disk2.slot] != CardKind::kDisk2) {
        return "";
    }
    const Disk2State& card = a.slots.disk2;
    std::string text = "disk: drive " + std::to_string(card.drive + 1);
    int track = DriveTrack(a, card.drive);
    text += track >= 0 ? " track " + std::to_string(track) : " past the last track";
    text += card.motor_on ? ", motor on" : ", motor off";
    for (int d = 0; d < 2; d++) {
        if (card.drives[d].dirty) {
            text += ", drive " + std::to_string(d + 1) + " written";
        }
    }
    return text + "\n";
}

} // namespace tisple
