// booting: our rom, our loader, snapshots mid-boot, and Apple's roms when the owner has them

#include "disk/boot_loader.hpp"
#include "machine/disk2.hpp"
#include "machine/slots.hpp"
#include "test_backend.hpp"
#include "test_disk.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tisple_tests {

namespace {

const char* const kBootProgram = R"(
; the text page and a buffer filled, then a sum, for the boot tests
(let sum u16)
(let buf (array u8 300))

(fn main () void
    (for i u16 0 1024
        (store (+ (as (ptr u8) 0x0400) i) (as u8 (+ (& i 0x3F) 0x80))))
    (for i u16 0 300
        (set (at buf i) (as u8 (* i 7)))
        (set sum (+ sum (* i 3)))))
)";

constexpr uint64_t kBootBudget = 40 * 1000 * 1000; // 40 M cycles: far more than a boot takes

struct BootCase {
    Compiled program;
    std::unique_ptr<tisple::DiskSectors> disk;
    tisple::BootDiskInfo info;
};

bool MakeCase(TestResults& results, const char* text, BootCase& out) {
    out.program = CompileText(text);
    CHECK(results, out.program.ok);
    if (!out.program.ok) {
        std::fprintf(stderr, "%s\n", out.program.errors.c_str());
        return false;
    }
    out.disk = std::make_unique<tisple::DiskSectors>();
    std::string error;
    bool made = tisple::MakeBootDisk(out.program.code.image, out.program.code.origin, *out.disk,
                                     out.info, error);
    CHECK(results, made);
    if (!made) {
        std::fprintf(stderr, "%s\n", error.c_str());
    }
    return made;
}

// the same program loaded straight into ram and run to its end
std::unique_ptr<tisple::Apple2> DirectRun(const BootCase& c) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    std::string error;
    tisple::LoadProgram(*a, c.program.code.image.data(), c.program.code.image.size(),
                        c.program.code.origin, error);
    tisple::RunApple2(*a, kBootBudget);
    return a;
}

uint64_t RunUntilStop(tisple::Apple2& a, uint64_t budget) {
    uint64_t start = tisple::Cycles(a);
    while (!a.stopped && tisple::Cycles(a) - start < budget) {
        tisple::RunApple2(a, tisple::kCyclesPerFrame);
    }
    return tisple::Cycles(a) - start;
}

// what the program left: the text page and its own bytes past the code
bool SameResult(const tisple::Apple2& a, const tisple::Apple2& b, const BootCase& c) {
    uint32_t origin = c.program.code.origin;
    uint32_t end = origin + static_cast<uint32_t>(c.program.code.image.size());
    bool text = std::memcmp(a.cpu.mem + 0x0400, b.cpu.mem + 0x0400, 0x0400) == 0;
    bool image = std::memcmp(a.cpu.mem + origin, b.cpu.mem + origin, end - origin) == 0;
    return text && image;
}

void TestOurRom(TestResults& results) {
    uint8_t rom[256];
    tisple::BuildDisk2BootRom(6, rom);
    // the autostart signature, 16 sectors, and the read entry at $C65C
    CHECK(results, rom[1] == 0x20 && rom[3] == 0x00 && rom[5] == 0x03 && rom[7] == 0x3C);
    CHECK(results, rom[0xFF] == 0x00);
    CHECK(results, rom[0x00] != 0x00 && rom[0x5C] != 0x00);
    uint8_t other[256];
    tisple::BuildDisk2BootRom(5, other);
    CHECK(results, std::memcmp(rom, other, 256) != 0); // the slot is built in
}

void TestBootOurRom(TestResults& results) {
    BootCase c;
    if (!MakeCase(results, kBootProgram, c)) {
        return;
    }
    std::unique_ptr<tisple::Apple2> direct = DirectRun(c);
    CHECK(results, direct->stop_reason == tisple::MachineStop::kReturned);

    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*c.disk);
    uint16_t return_pc = 0;
    CHECK(results, tisple::FindBootLoader(a->slots.disk2.drives[0].tracks[0], return_pc));
    CHECK(results, return_pc == c.info.return_pc);
    a->return_pc = return_pc;
    tisple::StartDiskBoot(*a);
    uint64_t took = RunUntilStop(*a, kBootBudget);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, a->stop_pc == c.info.return_pc);
    CHECK(results, SameResult(*a, *direct, c));
    // the program is one track: a seek, two turns, and the rom's two sectors (about a second)
    CHECK(results, took < 2 * 1000 * 1000);
    // the rom's nibble table, where boot sectors may look for it
    bool table = true;
    for (int n = 0x80; n < 0x100; n++) {
        int six = tisple::SixOfNibble(static_cast<uint8_t>(n));
        if (six >= 0 && a->cpu.mem[0x02D6 + n] != six) {
            table = false;
        }
    }
    CHECK(results, table);
    CHECK(results, a->cpu.mem[0x2B] == 0x60);
    CHECK(results, !tisple::DriveSpinning(*a, tisple::Cycles(*a) + tisple::kMotorOffDelay));
}

// a program over many tracks, at a low origin, loads every page in place
void TestBootBigProgram(TestResults& results) {
    std::vector<uint8_t> program(0xB000 - 0x0A40);
    uint32_t x = 12345;
    for (uint8_t& b : program) {
        x = x * 1103515245 + 12345;
        b = static_cast<uint8_t>(x >> 16);
    }
    program[0] = 0x60; // RTS at the origin
    tisple::DiskSectors disk;
    tisple::BootDiskInfo info;
    std::string error;
    CHECK(results, tisple::MakeBootDisk(program, 0x0A40, disk, info, error));
    CHECK(results, info.first_page == 0x0A && info.last_track == 11);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(disk);
    a->return_pc = info.return_pc;
    tisple::StartDiskBoot(*a);
    RunUntilStop(*a, kBootBudget);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, std::memcmp(a->cpu.mem + 0x0A40, program.data(), program.size()) == 0);
    CHECK(results, a->cpu.mem[0x0A3F] == 0 && a->cpu.mem[0xB000] == 0);

    // too big or too low is refused
    CHECK(results, !tisple::MakeBootDisk(program, 0x0900, disk, info, error));
    std::vector<uint8_t> huge(0xC000 - 0x0A00 + 1, 0x60);
    CHECK(results, !tisple::MakeBootDisk(huge, 0x0A00, disk, info, error));
}

// boot, snapshot halfway through the load, restore elsewhere: the same end
void TestBootSnapshot(TestResults& results) {
    BootCase c;
    if (!MakeCase(results, kBootProgram, c)) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*c.disk);
    a->return_pc = c.info.return_pc;
    tisple::StartDiskBoot(*a);
    tisple::RunApple2(*a, 400000); // the rom has read track 0; the loader is reading track 1
    CHECK(results, !a->stopped && tisple::DriveSpinning(*a, tisple::Cycles(*a)));
    std::vector<uint8_t> snapshot = tisple::SaveSnapshot(*a);
    CHECK(results, snapshot.size() < 900 * 1024);
    RunUntilStop(*a, kBootBudget);

    std::unique_ptr<tisple::Apple2> b = tisple::NewApple2();
    std::string error;
    CHECK(results, tisple::LoadSnapshot(*b, snapshot.data(), snapshot.size(), error));
    RunUntilStop(*b, kBootBudget);
    CHECK(results, b->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, tisple::Cycles(*a) == tisple::Cycles(*b));
    CHECK(results, std::memcmp(a->cpu.mem, b->cpu.mem, 0x10000) == 0);
    CHECK(results, SameResult(*a, *b, c));
}

// Apple's roms, when the owner has them: their autostart finds the card, their disk rom
// boots our loader; and their autostart with our disk rom
void TestBootAppleRoms(TestResults& results) {
    std::vector<uint8_t> main_rom = OwnerRom("Apple2e.rom");
    std::vector<uint8_t> disk_rom = OwnerRom("DISK2.rom");
    if (main_rom.empty() || disk_rom.size() != 256) {
        std::printf("disk: Apple ROMs not found, skipping the Apple ROM boot\n");
        return;
    }
    BootCase c;
    if (!MakeCase(results, kBootProgram, c)) {
        return;
    }
    std::unique_ptr<tisple::Apple2> direct = DirectRun(c);
    for (int ours = 0; ours < 2; ours++) {
        std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*c.disk);
        std::string error;
        CHECK(results, tisple::LoadRom(*a, main_rom.data(), main_rom.size(), error));
        if (ours == 0) {
            std::memcpy(a->slots.disk2.rom, disk_rom.data(), 256);
        }
        a->return_pc = c.info.return_pc;
        tisple::ResetApple2(*a);
        RunUntilStop(*a, kBootBudget);
        CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
        CHECK(results, a->stop_pc == c.info.return_pc);
        // the rom's banner sits on the top line until the program draws over it
        CHECK(results, SameResult(*a, *direct, c));
    }
}

// the text screen as one string, a line per row
std::string ScreenText(const tisple::Apple2& a) {
    std::string text;
    for (int row = 0; row < 24; row++) {
        int base = 0x0400 + (row % 8) * 0x80 + (row / 8) * 0x28;
        for (int col = 0; col < 40; col++) {
            int c = a.cpu.mem[base + col] & 0x7F;
            text += static_cast<char>(c < 0x20 ? c + 0x40 : c);
        }
        text += '\n';
    }
    return text;
}

// Zellyn Hunter's a2audit (scripts/fetch_a2audit.sh) through Apple's roms: its language card
// and soft switch tests pass. its aux memory test is skipped (no aux memory yet) and its
// video tests need a person to look
void TestA2Audit(TestResults& results) {
    std::vector<uint8_t> main_rom = OwnerRom("Apple2e.rom");
    std::vector<uint8_t> disk_rom = OwnerRom("DISK2.rom");
    std::vector<uint8_t> audit;
    std::string path = std::string(TISPLE_SOURCE_DIR) + "/tests/data/a2audit/audit.dsk";
    if (main_rom.empty() || disk_rom.size() != 256 || !tisple::ReadWholeFile(path, audit)) {
        std::printf("disk: a2audit or Apple ROMs not found, skipping (scripts/fetch_a2audit.sh)\n");
        return;
    }
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    std::string error;
    CHECK(results, tisple::LoadRom(*a, main_rom.data(), main_rom.size(), error));
    tisple::InsertDisk2Card(*a, 6, disk_rom.data());
    CHECK(results, tisple::InsertDiskImage(*a, 0, audit, tisple::DiskFormat::kDos, true, error));
    tisple::ResetApple2(*a);
    tisple::RunApple2(*a, 1500ull * tisple::kCyclesPerFrame);
    std::string screen = ScreenText(*a);
    bool lc = screen.find("LANGUAGE CARD TESTS SUCCEEDED") != std::string::npos;
    bool switches = screen.find("SOFTSWITCH TESTS SUCCEEDED") != std::string::npos;
    CHECK(results, lc);
    CHECK(results, switches);
    if (!lc || !switches) {
        std::fprintf(stderr, "a2audit screen:\n%s", screen.c_str());
    }
}

} // namespace

void RunDiskBootTests(TestResults& results) {
    TestOurRom(results);
    TestBootOurRom(results);
    TestBootBigProgram(results);
    TestBootSnapshot(results);
    TestBootAppleRoms(results);
    TestA2Audit(results);
}

} // namespace tisple_tests
