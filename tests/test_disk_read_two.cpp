// the second disk (tables :on disk2): where the build puts them, their four-byte records, the
// reads from drive 2, and a machine with one drive: drive 2 gives NO_DISK instead of hanging,
// then the second disk swapped into drive 1 reads there (disk.disk2_in 1); and its boot sector

#include "disk/disk_command.hpp"
#include "disk/disk_reader.hpp"
#include "disk/disk_tables.hpp"
#include "disk/image.hpp"
#include "machine/disk2.hpp"
#include "test_disk.hpp"
#include "test_disk_read.hpp"

#include <cstdio>
#include <cstring>

namespace tisple_tests {

namespace {

// the slots of the second disk's row: every other physical sector, round twice
void TestDisk2Places(TestResults& results) {
    std::vector<tisple::DiskTable> tables(4);
    tables[0].bytes.assign(300, 1); // 2 sectors, disk 2
    tables[0].disk = 2;
    tables[1].bytes.assign(512, 2);      // the boot disk's, as before
    tables[2].bytes.assign(20 * 256, 3); // 20 sectors, disk 2: from slot 2 of track 1 on
    tables[2].disk = 2;
    tables[3].bytes.assign(1, 4); // disk 2: slot 6 of track 2
    tables[3].disk = 2;
    std::string error;
    CHECK(results, tisple::PlaceDiskTables(tables, error));
    CHECK(results,
          tables[0].tracks[0] == 1 && tables[0].sectors[0] == 0 && tables[0].sectors[1] == 2);
    CHECK(results, tables[1].tracks[0] == 34 && tables[1].sectors[0] == 0);
    CHECK(results, tables[2].tracks[0] == 1 && tables[2].sectors[0] == 4);
    // slot 8 is physical sector 1; the 15th piece is the first of track 2
    CHECK(results,
          tables[2].sectors[6] == 1 && tables[2].tracks[14] == 2 && tables[2].sectors[14] == 0);
    CHECK(results, tables[3].tracks[0] == 2 && tables[3].sectors[0] == 12);
    std::vector<uint8_t> record = tisple::DiskTableRecord(tables[2]);
    CHECK(results, record.size() == 4 && record[0] == 0 && record[1] == 20 &&
                       record[2] == (1 | tisple::kDisk2Record) && record[3] == 2);
    CHECK(results, tisple::DiskTableRecordSize(20 * 256, 2) == 4);
    CHECK(results, tisple::DiskTablesFirstTrack(tables) == 34);
    CHECK(results, tisple::Disk2LastTrack(tables) == 2);
    // the second disk holds 544 sectors: tracks 1-34, no catalog
    std::vector<tisple::DiskTable> full(2);
    full[0].bytes.assign(40000, 0);
    full[0].disk = 2;
    full[1].bytes.assign(40000, 0);
    full[1].disk = 2;
    CHECK(results, tisple::PlaceDiskTables(full, error));
    CHECK(results, full[1].tracks.back() == 20);
    full.resize(4, full[0]);
    CHECK(results, !tisple::PlaceDiskTables(full, error) && error.find("544") != std::string::npos);
}

void TestDisk2CheckErrors(TestResults& results) {
    const char* text = "(const T (array u8 2) (make (array u8 2) 1 2) :on disk2 :interleave 3)\n"
                       "(fn main () void)";
    tisple::LoadOptions load;
    load.files.push_back(tisple::VirtualFile{"t.tsp", text});
    tisple::Program program;
    std::vector<tisple::Diagnostic> errors;
    bool ok = tisple::BuildProgram("t.tsp", load, program, errors);
    std::string shown = tisple::FormatDiagnostics(program.sources, errors);
    CHECK(results, !ok && shown.find("drop :interleave") != std::string::npos);
}

const uint32_t kSizes2[] = {1, 300, 4097, 9000, 256};
constexpr int kTables2 = 5;
constexpr uint16_t kStatus = 0x03C0;
constexpr uint16_t kGo = 0x03DF; // the test sets it once the disks are swapped
constexpr uint8_t kUntouched = 0xA5;

// tables on both disks; each read's status in kStatus + n. With swap: the first disk-2 read
// (from drive 2, none on a machine with one drive) gives NO_DISK, the program says the second
// disk is in drive 1 and waits for kGo, then reads them all from there
std::string TwoDiskProgram(const std::vector<std::vector<uint8_t>>& tables, bool swap) {
    std::string text = "(import disk)\n";
    text += "(let status (array u8 31) :address 0x03C0)\n";
    text += "(let room (array u8 25600) :address 0x2000 :in aux :reserve)\n";
    text += "(const BOOT (array u8 700) " + MakeBytes(DiskPattern(700, 99)) + " :on disk)\n";
    for (int k = 0; k < kTables2; k++) {
        text += "(const T" + std::to_string(k) + " (array u8 " + std::to_string(kSizes2[k]) + ") " +
                MakeBytes(tables[static_cast<size_t>(k)]) + " :on disk2)\n";
    }
    text += "(fn main () void\n";
    if (swap) {
        text += "    (set (at status 20) (disk.read (as (ptr u8) 0x2000) (addr_of (at T0 0)) "
                "disk.AUX))\n";
        text += "    (disk.disk2_in 1)\n";
        text += "    (while (= (io_read u8 0x03DF) 0))\n";
    } else {
        text += "    (set (at status 21) (disk.read (as (ptr u8) 0x7000) (addr_of (at BOOT 0)) "
                "disk.AUX))\n";
    }
    uint32_t at = 0x2000;
    for (int k = 0; k < kTables2; k++) {
        text += "    (set (at status " + std::to_string(k) + ") (disk.read (as (ptr u8) " +
                std::to_string(at) + ") (addr_of (at T" + std::to_string(k) + " 0)) disk.AUX))\n";
        at += kSizes2[k] + 1;
    }
    if (!swap) {
        // back to the boot disk's drive after the second's: drive 1's head is found again
        text += "    (set (at status 22) (disk.read (as (ptr u8) 0x7400) (addr_of (at BOOT 0)) "
                "disk.AUX))\n";
    }
    return text + ")\n";
}

bool TablesIn(const tisple::Apple2& a, const std::vector<std::vector<uint8_t>>& tables) {
    uint32_t at = 0x2000;
    bool ok = true;
    for (int k = 0; k < kTables2; k++) {
        const std::vector<uint8_t>& want = tables[static_cast<size_t>(k)];
        ok = ok && std::memcmp(a.aux + at, want.data(), want.size()) == 0 &&
             a.aux[at + want.size()] == kUntouched;
        ok = ok && a.cpu.mem[kStatus + k] == tisple::kReaderDone;
        at += kSizes2[k] + 1;
    }
    return ok;
}

std::unique_ptr<tisple::Apple2> BootTwo(const DiskProgram& p, const tisple::DiskSectors& disk2,
                                        bool one_drive) {
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    if (one_drive) {
        a->slots.disk2.drives[1].missing = true;
    } else {
        std::vector<uint8_t> file = tisple::ImageFromSectors(disk2, tisple::SectorOrder::kDos);
        std::string error;
        tisple::InsertDiskImage(*a, 1, file, tisple::DiskFormat::kDos, false, error);
    }
    for (uint32_t i = 0x2000; i < 0x8000; i++) {
        a->aux[i] = kUntouched;
    }
    return a;
}

void TestTwoDrives(TestResults& results, const std::vector<std::vector<uint8_t>>& tables) {
    DiskProgram p;
    BuildDiskProgram(results, TwoDiskProgram(tables, false), DiskReadLayout(0xA800), p);
    if (!p.ok) {
        return;
    }
    tisple::DiskSectors disk2;
    tisple::MakeDisk2(p.program.disk_tables, disk2);
    std::unique_ptr<tisple::Apple2> a = BootTwo(p, disk2, false);
    uint64_t took = RunDiskProgram(*a, kDiskReadBudget);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, TablesIn(*a, tables));
    std::vector<uint8_t> boot = DiskPattern(700, 99);
    CHECK(results, a->cpu.mem[kStatus + 21] == tisple::kReaderDone &&
                       a->cpu.mem[kStatus + 22] == tisple::kReaderDone &&
                       std::memcmp(a->aux + 0x7000, boot.data(), boot.size()) == 0 &&
                       std::memcmp(a->aux + 0x7400, boot.data(), boot.size()) == 0);
    std::printf("disk read, two drives: %d tables from drive 2 between two from drive 1, %.1f s "
                "from power on\n",
                kTables2, static_cast<double>(took) / 1023000.0);
}

void TestOneDrive(TestResults& results, const std::vector<std::vector<uint8_t>>& tables) {
    DiskProgram p;
    BuildDiskProgram(results, TwoDiskProgram(tables, true), DiskReadLayout(0xA800), p);
    if (!p.ok) {
        return;
    }
    tisple::DiskSectors disk2;
    tisple::MakeDisk2(p.program.disk_tables, disk2);
    std::unique_ptr<tisple::Apple2> a = BootTwo(p, disk2, true);
    a->cpu.mem[kGo] = 0;
    // the boot, then drive 2's NO_DISK: nothing comes from it, and the read stops
    uint64_t start = tisple::Cycles(*a);
    uint64_t gave_up = 0;
    while (tisple::Cycles(*a) - start < kDiskReadBudget && gave_up == 0) {
        tisple::RunApple2(*a, tisple::kCyclesPerFrame);
        if (a->cpu.mem[kStatus + 20] == tisple::kReaderNoDisk) {
            gave_up = tisple::Cycles(*a);
        }
    }
    CHECK(results, gave_up != 0);
    // the person swaps: the second disk in drive 1
    std::vector<uint8_t> file = tisple::ImageFromSectors(disk2, tisple::SectorOrder::kDos);
    std::string error;
    CHECK(results, tisple::InsertDiskImage(*a, 0, file, tisple::DiskFormat::kDos, false, error));
    uint64_t swapped = tisple::Cycles(*a);
    a->cpu.mem[kGo] = 1;
    RunDiskProgram(*a, kDiskReadBudget);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, TablesIn(*a, tables));
    std::printf("disk read, one drive: drive 2 gave NO_DISK, then the second disk read from "
                "drive 1 in %.1f s\n",
                static_cast<double>(tisple::Cycles(*a) - swapped) / 1023000.0);
}

// booted by mistake, the second disk says which to boot, on the text screen
void TestDisk2Boot(TestResults& results) {
    std::vector<tisple::DiskTable> tables(1);
    tables[0].bytes.assign(10, 7);
    tables[0].disk = 2;
    std::string error;
    CHECK(results, tisple::PlaceDiskTables(tables, error));
    tisple::DiskSectors disk2;
    tisple::MakeDisk2(tables, disk2);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(disk2);
    tisple::StartDiskBoot(*a);
    for (int f = 0; f < 120; f++) {
        tisple::RunApple2(*a, tisple::kCyclesPerFrame);
    }
    const char* want = "THIS IS DISK 2: BOOT DISK 1";
    size_t n = std::strlen(want);
    uint16_t at = static_cast<uint16_t>(0x0628 + (40 - n) / 2);
    bool shown = true;
    for (size_t i = 0; i < n; i++) {
        shown = shown && a->cpu.mem[at + i] == (static_cast<uint8_t>(want[i]) | 0x80);
    }
    CHECK(results, shown && a->cpu.mem[0x0400] == 0xA0 && !a->stopped);
    CHECK(results, disk2.data[1][0][0] == 7);
}

} // namespace

void RunDiskReadTwoTests(TestResults& results) {
    TestDisk2Places(results);
    TestDisk2CheckErrors(results);
    std::vector<std::vector<uint8_t>> tables;
    for (int k = 0; k < kTables2; k++) {
        tables.push_back(DiskPattern(kSizes2[k], static_cast<uint32_t>(50 + k)));
    }
    TestTwoDrives(results, tables);
    TestOneDrive(results, tables);
    TestDisk2Boot(results);
}

} // namespace tisple_tests
