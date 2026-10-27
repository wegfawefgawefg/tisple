#pragma once

#include "check.hpp"
#include "disk/image.hpp"
#include "machine/apple2.hpp"

#include <memory>
#include <string>
#include <vector>

namespace tisple_tests {

// disks: the 16-sector format, image files, DOS 3.3, the disk ii card, booting
void RunDiskTests(TestResults& results);

void RunDiskFormatTests(TestResults& results);
void RunDiskDriveTests(TestResults& results);
void RunDiskBootTests(TestResults& results);
void RunDiskSegmentTests(TestResults& results);

// a disk of reproducible noise: sector bytes from a seed
std::unique_ptr<tisple::DiskSectors> NoiseDisk(uint32_t seed);

// a machine with the disk ii card in slot 6 (our rom) and these sectors in drive 1
std::unique_ptr<tisple::Apple2> MachineWithDisk(const tisple::DiskSectors& sectors);

// the owner's rom files, or empty when they are not there (tests then skip)
std::vector<uint8_t> OwnerRom(const char* name);

} // namespace tisple_tests
