// the 16-sector format, image files and DOS 3.3

#include "disk/disk_command.hpp"
#include "disk/dos33.hpp"
#include "machine/disk2.hpp"
#include "test_disk.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unistd.h>

namespace tisple_tests {

std::unique_ptr<tisple::DiskSectors> NoiseDisk(uint32_t seed) {
    std::unique_ptr<tisple::DiskSectors> disk = std::make_unique<tisple::DiskSectors>();
    uint32_t x = seed;
    for (auto& track : disk->data) {
        for (auto& sector : track) {
            for (uint8_t& b : sector) {
                x = x * 1664525u + 1013904223u;
                b = static_cast<uint8_t>(x >> 24);
            }
        }
    }
    return disk;
}

std::unique_ptr<tisple::Apple2> MachineWithDisk(const tisple::DiskSectors& sectors) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    tisple::InsertDisk2Card(*a, 6, nullptr);
    std::vector<uint8_t> file = tisple::ImageFromSectors(sectors, tisple::SectorOrder::kDos);
    std::string error;
    tisple::InsertDiskImage(*a, 0, file, tisple::DiskFormat::kDos, false, error);
    return a;
}

std::vector<uint8_t> OwnerRom(const char* name) {
    std::vector<uint8_t> bytes;
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return bytes;
    }
    std::string path = std::string(home) + "/.local/share/tisple/roms/" + name;
    if (!tisple::ReadWholeFile(path, bytes)) {
        bytes.clear();
    }
    return bytes;
}

namespace {

struct Tracks {
    tisple::DiskTrack t[tisple::kDiskTracks];
};

void TestNibbleTable(TestResults& results) {
    bool unique = true;
    bool valid = true;
    for (int v = 0; v < 64; v++) {
        uint8_t n = tisple::NibbleOf6(v);
        unique = unique && tisple::SixOfNibble(n) == v;
        valid = valid && (n & 0x80) != 0 && n != 0xD5 && n != 0xAA;
    }
    CHECK(results, unique && valid);
    CHECK(results, tisple::NibbleOf6(0) == 0x96 && tisple::NibbleOf6(63) == 0xFF);
    int count = 0;
    for (int n = 0; n < 256; n++) {
        count += tisple::SixOfNibble(static_cast<uint8_t>(n)) >= 0 ? 1 : 0;
    }
    CHECK(results, count == 64);
}

void Test44(TestResults& results) {
    bool all = true;
    for (int v = 0; v < 256; v++) {
        uint8_t n[2];
        tisple::Encode44(static_cast<uint8_t>(v), n);
        all = all && (n[0] & 0xAA) == 0xAA && (n[1] & 0xAA) == 0xAA &&
              tisple::Decode44(n[0], n[1]) == v;
    }
    CHECK(results, all);
}

// every byte value at every position, both ways; and what DOS 3.3 puts on the disk
void Test62(TestResults& results) {
    bool all = true;
    for (int v = 0; v < 256; v++) {
        uint8_t sector[256];
        for (int i = 0; i < 256; i++) {
            sector[i] = static_cast<uint8_t>(v + i * 37);
        }
        uint8_t nibbles[tisple::kEncodedSectorNibbles];
        tisple::Encode62(sector, nibbles);
        uint8_t back[256];
        all = all && tisple::Decode62(nibbles, back) && std::memcmp(sector, back, 256) == 0;
        for (uint8_t n : nibbles) {
            all = all && tisple::SixOfNibble(n) >= 0;
        }
    }
    CHECK(results, all);
    // a zero sector is 343 $96s; one bad nibble or a wrong checksum is caught
    uint8_t zero[256] = {};
    uint8_t nibbles[tisple::kEncodedSectorNibbles];
    tisple::Encode62(zero, nibbles);
    bool all96 = true;
    for (uint8_t n : nibbles) {
        all96 = all96 && n == 0x96;
    }
    CHECK(results, all96);
    // byte 0 = 1: its low bits are swapped into the first low-bits entry (value 2)
    zero[0] = 0x01;
    tisple::Encode62(zero, nibbles);
    CHECK(results, nibbles[0] == tisple::NibbleOf6(2) && nibbles[1] == tisple::NibbleOf6(2));
    uint8_t back[256];
    nibbles[100] = 0xD5;
    CHECK(results, !tisple::Decode62(nibbles, back));
    tisple::Encode62(zero, nibbles);
    nibbles[342] = tisple::NibbleOf6(5);
    CHECK(results, !tisple::Decode62(nibbles, back));
}

void TestSectorOrders(TestResults& results) {
    // DOS 3.3 logical sector 1 is physical 13; ProDOS block 0 is physical 0 and 2
    CHECK(results, tisple::DosLogicalOfPhysical(13) == 1);
    CHECK(results, tisple::PhysicalOfDosLogical(1) == 13);
    CHECK(results, tisple::FileSectorOfPhysical(tisple::SectorOrder::kProdos, 2) == 1);
    bool inverse = true;
    for (int l = 0; l < 16; l++) {
        inverse = inverse && tisple::DosLogicalOfPhysical(tisple::PhysicalOfDosLogical(l)) == l;
    }
    CHECK(results, inverse);
}

// sectors to nibbled tracks and back, both orders, and .nib
void TestImageRoundTrips(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> disk = NoiseDisk(7);
    auto tracks = std::make_unique<Tracks>();
    auto& t = tracks->t;
    std::string error;
    for (tisple::SectorOrder order : {tisple::SectorOrder::kDos, tisple::SectorOrder::kProdos}) {
        tisple::DiskFormat format = order == tisple::SectorOrder::kDos
                                        ? tisple::DiskFormat::kDos
                                        : tisple::DiskFormat::kProdos;
        std::vector<uint8_t> file = tisple::ImageFromSectors(*disk, order);
        CHECK(results, file.size() == tisple::kSectorImageBytes);
        CHECK(results, tisple::TracksFromImage(file, format, t, error));
        CHECK(results, t[0].bit_count == 51264);
        std::vector<uint8_t> back;
        CHECK(results, tisple::ImageFromTracks(t, format, back, error));
        CHECK(results, back == file);
        tisple::DiskSectors again;
        CHECK(results, tisple::SectorsFromImage(back, order, again, error));
        CHECK(results, std::memcmp(again.data, disk->data, sizeof(again.data)) == 0);
    }
    // the same disk in either order puts the same bits on the track
    std::vector<uint8_t> dos = tisple::ImageFromSectors(*disk, tisple::SectorOrder::kDos);
    std::vector<uint8_t> prodos = tisple::ImageFromSectors(*disk, tisple::SectorOrder::kProdos);
    CHECK(results, dos != prodos);
    auto other = std::make_unique<Tracks>();
    auto& u = other->t;
    tisple::TracksFromImage(dos, tisple::DiskFormat::kDos, t, error);
    tisple::TracksFromImage(prodos, tisple::DiskFormat::kProdos, u, error);
    CHECK(results, std::memcmp(t[5].bits, u[5].bits, sizeof(t[5].bits)) == 0);

    // .nib: dos tracks as raw bytes; reading those back gives the sectors again
    std::vector<uint8_t> nib;
    CHECK(results, tisple::ImageFromTracks(t, tisple::DiskFormat::kNib, nib, error));
    CHECK(results, nib.size() == tisple::kNibImageBytes);
    CHECK(results, tisple::TracksFromImage(nib, tisple::DiskFormat::kNib, u, error));
    CHECK(results, u[0].bit_count == tisple::kMaxTrackBits);
    std::vector<uint8_t> from_nib;
    CHECK(results, tisple::ImageFromTracks(u, tisple::DiskFormat::kDos, from_nib, error));
    CHECK(results, from_nib == dos);
    std::vector<uint8_t> nib_again;
    CHECK(results, tisple::ImageFromTracks(u, tisple::DiskFormat::kNib, nib_again, error));
    CHECK(results, nib_again == nib);

    // a damaged track is named, not saved
    t[3].bits[2000] ^= 0x10;
    std::vector<uint8_t> bad;
    CHECK(results, !tisple::ImageFromTracks(t, tisple::DiskFormat::kDos, bad, error));
    CHECK(results, error.find("track 3") != std::string::npos);
    CHECK(results,
          !tisple::TracksFromImage(std::vector<uint8_t>(1000), tisple::DiskFormat::kDos, t, error));
    CHECK(results, tisple::DiskFormatForPath("a/GAME.DSK") == tisple::DiskFormat::kDos);
    CHECK(results, tisple::DiskFormatForPath("x.po") == tisple::DiskFormat::kProdos);
    CHECK(results, tisple::DiskFormatForPath("x.nib") == tisple::DiskFormat::kNib);
    CHECK(results, tisple::DiskFormatForPath("x.bin") == tisple::DiskFormat::kNone);
}

void TestDos33(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> disk = std::make_unique<tisple::DiskSectors>();
    tisple::FormatDos33(*disk, 254);
    CHECK(results, tisple::IsDos33(*disk));
    CHECK(results, tisple::DosFreeSectors(*disk) == 33 * 16);
    std::vector<tisple::DosFile> files;
    std::string error;
    CHECK(results, tisple::DosCatalog(*disk, files, error) && files.empty());

    std::vector<uint8_t> code(1000);
    for (size_t i = 0; i < code.size(); i++) {
        code[i] = static_cast<uint8_t>(i * 13);
    }
    CHECK(results,
          tisple::DosPutFile(*disk, "game", 'B', tisple::DosBinaryContents(code, 0x6000), error));
    CHECK(results, tisple::DosPutFile(*disk, "README", 'T',
                                      tisple::DosTextContents("HELLO\nWORLD\n"), error));
    CHECK(results, !tisple::DosPutFile(*disk, "GAME", 'B', code, error)); // already there
    // 1004 bytes: 4 data sectors and a list; the text: one and one
    CHECK(results, tisple::DosFreeSectors(*disk) == 33 * 16 - 7);
    files.clear();
    CHECK(results, tisple::DosCatalog(*disk, files, error) && files.size() == 2);
    CHECK(results, files[0].name == "GAME" && files[0].type == 'B' && files[0].sectors == 5);
    CHECK(results, files[1].name == "README" && files[1].type == 'T' && files[1].sectors == 2);
    // DOS starts filling at track 18, from sector 15 down
    CHECK(results, disk->data[18][tisple::PhysicalOfDosLogical(15)][0x0C] == 18);

    tisple::DosFile file;
    std::vector<uint8_t> contents;
    CHECK(results, tisple::DosGetFile(*disk, "GAME", file, contents, error));
    uint16_t address = 0;
    std::vector<uint8_t> data;
    CHECK(results, tisple::DosBinaryData(contents, address, data));
    CHECK(results, address == 0x6000 && data == code);
    CHECK(results, tisple::DosGetFile(*disk, "readme", file, contents, error));
    CHECK(results, tisple::DosTextData(contents) == "HELLO\nWORLD\n");

    CHECK(results, tisple::DosDeleteFile(*disk, "GAME", error));
    CHECK(results, !tisple::DosGetFile(*disk, "GAME", file, contents, error));
    CHECK(results, tisple::DosFreeSectors(*disk) == 33 * 16 - 2);
    files.clear();
    CHECK(results, tisple::DosCatalog(*disk, files, error) && files.size() == 1);
    // the deleted entry's slot is used again
    CHECK(results,
          tisple::DosPutFile(*disk, "AGAIN", 'B', tisple::DosBinaryContents(code, 0x800), error));
    files.clear();
    CHECK(results, tisple::DosCatalog(*disk, files, error) && files.size() == 2);
    CHECK(results, files[0].name == "AGAIN");

    // a file bigger than the disk is refused and changes nothing
    uint32_t free_before = tisple::DosFreeSectors(*disk);
    std::vector<uint8_t> huge(140 * 1024);
    CHECK(results, !tisple::DosPutFile(*disk, "HUGE", 'B', huge, error));
    CHECK(results, tisple::DosFreeSectors(*disk) == free_before);
    // a file of more than 122 sectors takes two lists
    std::vector<uint8_t> big(130 * 256);
    CHECK(results, tisple::DosPutFile(*disk, "BIG", 'B', big, error));
    CHECK(results, tisple::DosFreeSectors(*disk) == free_before - 132);
    CHECK(results, tisple::DosGetFile(*disk, "BIG", file, contents, error));
    CHECK(results, contents.size() == 130 * 256 && file.sectors == 132);

    std::unique_ptr<tisple::DiskSectors> noise = NoiseDisk(3);
    CHECK(results, !tisple::DosCatalog(*noise, files, error));
}

int Disk(std::vector<std::string> args) {
    args.insert(args.begin(), {"tisple", "disk"});
    std::vector<char*> argv;
    for (std::string& a : args) {
        argv.push_back(a.data());
    }
    return tisple::RunDiskCommand(static_cast<int>(argv.size()), argv.data());
}

// tisple disk on files: new, put, list, get, delete, boot, in both sector orders and .nib
void TestDiskCommand(TestResults& results) {
    std::string dir =
        (std::filesystem::temp_directory_path() / ("tisple_disk_" + std::to_string(getpid())))
            .string();
    std::filesystem::create_directories(dir);
    std::vector<uint8_t> code = {0xA9, 0x41, 0x8D, 0x00, 0x04, 0x60};
    CHECK(results, tisple::WriteWholeFile(dir + "/prog.bin", code));
    CHECK(results,
          tisple::WriteWholeFile(dir + "/notes.txt", std::vector<uint8_t>{'H', 'I', '\n'}));
    for (const char* ext : {".dsk", ".po", ".nib"}) {
        std::string image = dir + "/d" + ext;
        CHECK(results, Disk({"new", image}) == 0);
        CHECK(results, Disk({"put", image, dir + "/prog.bin", "--addr", "$0300"}) == 0);
        CHECK(results, Disk({"put", image, dir + "/notes.txt", "--type", "T"}) == 0);
        CHECK(results, Disk({"list", image}) == 0);
        CHECK(results, Disk({"get", image, "PROG", "-o", dir + "/out.bin"}) == 0);
        std::vector<uint8_t> back;
        CHECK(results, tisple::ReadWholeFile(dir + "/out.bin", back) && back == code);
        CHECK(results, Disk({"get", image, "NOTES", "-o", dir + "/out.txt"}) == 0);
        CHECK(results, tisple::ReadWholeFile(dir + "/out.txt", back) && back.size() == 3);
        CHECK(results, Disk({"delete", image, "PROG"}) == 0);
        CHECK(results, Disk({"get", image, "PROG", "-o", dir + "/out.bin"}) != 0);
        tisple::DiskSectors sectors;
        std::string error;
        std::vector<tisple::DosFile> files;
        CHECK(results, tisple::ReadDiskSectorsFile(image, sectors, error));
        CHECK(results, tisple::DosCatalog(sectors, files, error) && files.size() == 1);
    }
    // a boot disk is also a DOS 3.3 volume with room for files
    std::string boot = dir + "/boot.dsk";
    CHECK(results, Disk({"boot", boot, dir + "/prog.bin", "--origin", "0x2000"}) == 0);
    CHECK(results, Disk({"put", boot, dir + "/notes.txt", "--type", "T"}) == 0);
    tisple::DiskSectors sectors;
    std::string error;
    CHECK(results, tisple::ReadDiskSectorsFile(boot, sectors, error));
    CHECK(results, tisple::DosFreeSectors(sectors) == 33 * 16 - 16 - 2);
    CHECK(results, std::memcmp(sectors.data[1][0], code.data(), code.size()) == 0);
    CHECK(results, Disk({"boot", boot, dir + "/prog.bin", "--origin", "0x0800"}) != 0);
    CHECK(results, Disk({"list", dir + "/missing.dsk"}) != 0);
    CHECK(results, Disk({"frob"}) != 0);
    std::filesystem::remove_all(dir);
}

} // namespace

void RunDiskFormatTests(TestResults& results) {
    TestNibbleTable(results);
    Test44(results);
    Test62(results);
    TestSectorOrders(results);
    TestImageRoundTrips(results);
    TestDos33(results);
    TestDiskCommand(results);
}

void RunDiskTests(TestResults& results) {
    RunDiskFormatTests(results);
    RunDiskDriveTests(results);
    RunDiskBootTests(results);
    RunDiskSegmentTests(results);
}

} // namespace tisple_tests
