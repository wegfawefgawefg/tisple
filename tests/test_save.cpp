// lib/save on the built-in machine: saves written by our writer and read back, boot after
// boot; the write's timing (a nibble every 32 cycles); the whole disk still reading and the
// tracks below the save areas never touched; the image written back to a .dsk; a power cut at
// any moment of a write; the write protect; the writer's refusals; a build without a disk

#include "disk/disk_tables.hpp"
#include "disk/disk_writer.hpp"
#include "disk/image.hpp"
#include "disk/nibble.hpp"
#include "machine/disk2.hpp"
#include "test_apple2_libs.hpp"
#include "test_disk.hpp"
#include "test_disk_read.hpp"

#include <cstdio>
#include <cstring>

namespace tisple_tests {

namespace {

// each boot reads the save (round r: byte i is i * 3 + r), checks it, writes round r + 1
std::string CycleProgram(int bytes) {
    std::string n = std::to_string(bytes);
    std::string room = std::to_string(bytes > 256 ? bytes : 257); // indexed by a u16
    return "(import save)\n"
           "(save.area GAME " +
           n +
           ")\n"
           "(let data (array u8 " +
           room +
           "))\n"
           "(let out (array u8 8) :address 0x0300)\n"
           "(fn main () void\n"
           "    (let got u8 (save.read (addr_of (at GAME 0)) (addr_of (at data 0)) " +
           n +
           "))\n"
           "    (set (at out 0) got)\n"
           "    (if (= got save.EMPTY)\n"
           "        (for i u16 0 " +
           n +
           "\n"
           "            (set (at data i) (as u8 (* i 3)))))\n"
           "    (let round u8 (at data 0))\n"
           "    (let good u8 1)\n"
           "    (for i u16 0 " +
           n +
           "\n"
           "        (if (!= (at data i) (+ (as u8 (* i 3)) round))\n"
           "            (set good 0)))\n"
           "    (set (at out 1) good)\n"
           "    (set (at out 2) round)\n"
           "    (for i u16 0 " +
           n +
           "\n"
           "        (set (at data i) (+ (at data i) 1)))\n"
           "    (set (at out 3) (save.write (addr_of (at GAME 0)) (addr_of (at data 0)) " +
           n +
           "))\n"
           "    (set (at out 4) 0xAA))\n";
}

constexpr uint8_t kOk = 0;
constexpr uint8_t kEmpty = 8;

struct Boot {
    int got = -1; // what read gave
    bool good = false;
    int round = -1;
    int wrote = -1; // what write gave
    bool finished = false;
    std::vector<uint64_t> loads; // cycles of the data register's loads
};

// the cycle each load of the data register happens (STA $C08D,X or $C08F,X: its last cycle)
void WatchLoads(void* ctx, const tisple::Machine& m) {
    std::vector<uint64_t>& loads = *static_cast<std::vector<uint64_t>*>(ctx);
    uint16_t pc = tisple::GetRegs(m).pc;
    if (m.mem[pc] == 0x9D && (m.mem[pc + 1] == 0x8D || m.mem[pc + 1] == 0x8F) &&
        m.mem[pc + 2] == 0xC0) {
        loads.push_back(m.cycles + 5);
    }
}

std::unique_ptr<tisple::Apple2> BootDrive(const tisple::DiskDrive& drive, uint16_t return_pc) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    tisple::InsertDisk2Card(*a, 6, nullptr);
    a->slots.disk2.drives[0] = drive;
    a->slots.disk2.drives[0].spun_to = 0;
    a->slots.disk2.drives[0].half_track = 0;
    a->return_pc = return_pc;
    tisple::StartDiskBoot(*a);
    return a;
}

Boot RunBoot(tisple::Apple2& a, uint64_t until = kDiskReadBudget) {
    Boot b;
    uint64_t start = tisple::Cycles(a);
    while (!a.stopped && tisple::Cycles(a) - start < until) {
        uint64_t step = until - (tisple::Cycles(a) - start);
        step = step < tisple::kCyclesPerFrame ? step : tisple::kCyclesPerFrame;
        tisple::RunApple2Traced(a, step, WatchLoads, &b.loads);
    }
    const uint8_t* out = a.cpu.mem + 0x0300;
    b.finished = out[4] == 0xAA && a.stop_reason == tisple::MachineStop::kReturned;
    b.got = out[0];
    b.good = out[1] == 1;
    b.round = out[2];
    b.wrote = out[3];
    return b;
}

// the loads in fields (more than 100 cycles between two: another field): each field's first 6
// loads are 40 cycles apart (5 self-sync bytes), the other 349 are 32 apart
bool FieldTimesRight(const std::vector<uint64_t>& loads, int& fields) {
    fields = 0;
    size_t i = 0;
    while (i < loads.size()) {
        size_t end = i + 1;
        while (end < loads.size() && loads[end] - loads[end - 1] < 100) {
            end++;
        }
        if (end - i != 355) {
            return false;
        }
        for (size_t k = i + 1; k < end; k++) {
            uint64_t want = k - i <= 5 ? 40 : 32;
            if (loads[k] - loads[k - 1] != want) {
                return false;
            }
        }
        fields++;
        i = end;
    }
    return true;
}

bool AllTracksRead(const tisple::DiskDrive& d) {
    for (int t = 0; t < tisple::kDiskTracks; t++) {
        uint8_t sectors[16][256];
        bool found[16];
        tisple::DenibblizeTrack(d.tracks[t], t, sectors, found);
        for (bool f : found) {
            if (!f) {
                return false;
            }
        }
    }
    return true;
}

bool SameTracksBelow(const tisple::DiskDrive& a, const tisple::DiskDrive& b, int floor) {
    for (int t = 0; t < floor; t++) {
        if (a.tracks[t].bit_count != b.tracks[t].bit_count ||
            std::memcmp(a.tracks[t].bits, b.tracks[t].bits, sizeof(a.tracks[t].bits)) != 0) {
            return false;
        }
    }
    return true;
}

int Floor(const DiskProgram& p) {
    return tisple::DiskTablesFirstTrack(p.program.disk_tables);
}

// boot after boot: empty, then rounds 1, 2, 3; each write's 2 or more fields timed right
void TestRounds(TestResults& results, int bytes) {
    DiskProgram p;
    BuildDiskProgram(results, CycleProgram(bytes), DiskReadLayout(0x6000), p);
    if (!p.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    tisple::DiskDrive first = a->slots.disk2.drives[0];
    int sectors = (bytes + 255) / 256 + 1;
    for (int round = 0; round < 4; round++) {
        Boot b = RunBoot(*a);
        int fields = 0;
        bool timed = FieldTimesRight(b.loads, fields);
        bool ok = b.finished && b.got == (round == 0 ? kEmpty : kOk) && b.good &&
                  b.round == (round == 0 ? 0 : round) && b.wrote == kOk && timed &&
                  fields == sectors;
        if (!ok) {
            std::fprintf(stderr, "save %d bytes, boot %d: read %d good %d round %d wrote %d, "
                                 "%d fields timed %d\n",
                         bytes, round, b.got, b.good ? 1 : 0, b.round, b.wrote, fields,
                         timed ? 1 : 0);
        }
        CHECK(results, ok);
        if (bytes == 300 && round == 1 && b.loads.size() > 1) {
            std::printf("save: 300 bytes, 3 sectors written in %.2f s, the first to the last load\n",
                        static_cast<double>(b.loads.back() - b.loads.front()) / 1020484.0);
        }
        CHECK(results, AllTracksRead(a->slots.disk2.drives[0]));
        CHECK(results, SameTracksBelow(first, a->slots.disk2.drives[0], Floor(p)));
        a = BootDrive(a->slots.disk2.drives[0], p.info.return_pc);
    }
}

// the drive as a .dsk (as --write-back saves it), into a fresh machine: the save is there
void TestImage(TestResults& results) {
    DiskProgram p;
    BuildDiskProgram(results, CycleProgram(300), DiskReadLayout(0x6000), p);
    if (!p.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    RunBoot(*a);
    RunBoot(*BootDrive(a->slots.disk2.drives[0], p.info.return_pc));
    std::vector<uint8_t> file;
    std::string error;
    CHECK(results, a->slots.disk2.drives[0].dirty);
    CHECK(results, tisple::SaveDiskImage(*a, 0, file, error));
    std::unique_ptr<tisple::Apple2> b = tisple::NewApple2();
    tisple::InsertDisk2Card(*b, 6, nullptr);
    CHECK(results, tisple::InsertDiskImage(*b, 0, file, tisple::DiskFormat::kDos, false, error));
    b->return_pc = p.info.return_pc;
    tisple::StartDiskBoot(*b);
    Boot again = RunBoot(*b);
    CHECK(results, again.finished && again.got == kOk && again.good && again.round == 1);
}

// a power cut at any moment of a write: the drive as it was then (and, while it was writing,
// the bits under the head for the next 200 cells made noise), booted again. The disk boots,
// the tracks below the save areas are as they were, and the read gives the round before or
// the new one, whole, never EMPTY or a mix
void TestPowerCut(TestResults& results) {
    DiskProgram p;
    BuildDiskProgram(results, CycleProgram(600), DiskReadLayout(0x6000), p);
    if (!p.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    tisple::DiskDrive original = a->slots.disk2.drives[0];
    RunBoot(*a); // round 1 saved
    tisple::DiskDrive saved = a->slots.disk2.drives[0];
    // a dry run of the next boot: when its write begins and ends
    std::unique_ptr<tisple::Apple2> dry = BootDrive(saved, p.info.return_pc);
    Boot whole = RunBoot(*dry);
    CHECK(results, whole.finished && whole.round == 1 && whole.loads.size() > 1000);
    if (whole.loads.size() < 2) {
        return;
    }
    // cuts across the whole save, then every 250 cycles through the header's field, the last
    uint64_t begin = whole.loads.front() - 20000;
    uint64_t end = whole.loads.back() + 20000;
    uint64_t header = whole.loads[whole.loads.size() - 355] - 500;
    std::vector<uint64_t> cut_at;
    for (int k = 0; k <= 40; k++) {
        cut_at.push_back(begin + (end - begin) * static_cast<uint64_t>(k) / 40);
    }
    for (uint64_t at = header; at < whole.loads.back() + 1000; at += 250) {
        cut_at.push_back(at);
    }
    int old_round = 0;
    int new_round = 0;
    int bad = 0;
    uint32_t noise = 0x1234567;
    for (uint64_t at : cut_at) {
        std::unique_ptr<tisple::Apple2> cut = BootDrive(saved, p.info.return_pc);
        RunBoot(*cut, at);
        tisple::DiskDrive drive = cut->slots.disk2.drives[0];
        if (cut->slots.disk2.q7) {
            tisple::DiskTrack& t = drive.tracks[drive.half_track / 2];
            for (uint32_t c = 0; c < 200; c++) {
                uint32_t pos = (drive.bit_pos + c) % t.bit_count;
                noise = noise * 1103515245u + 12345u;
                uint8_t mask = static_cast<uint8_t>(0x80 >> (pos & 7));
                t.bits[pos >> 3] = static_cast<uint8_t>(((noise >> 16) & 1) != 0
                                                            ? t.bits[pos >> 3] | mask
                                                            : t.bits[pos >> 3] & ~mask);
            }
        }
        bool below = SameTracksBelow(original, drive, Floor(p));
        Boot after = RunBoot(*BootDrive(drive, p.info.return_pc));
        bool ok = below && after.finished && after.got == kOk && after.good &&
                  (after.round == 1 || after.round == 2);
        old_round += after.round == 1 ? 1 : 0;
        new_round += after.round == 2 ? 1 : 0;
        if (!ok && bad++ < 4) {
            std::fprintf(stderr, "power cut at %llu: below %d, read %d good %d round %d\n",
                         static_cast<unsigned long long>(at - begin), below ? 1 : 0, after.got,
                         after.good ? 1 : 0, after.round);
        }
    }
    CHECK(results, bad == 0);
    CHECK(results, old_round > 0 && new_round > 0);
    std::printf("save: %zu power cuts in a write: %d read the save before, %d the new one\n",
                cut_at.size(), old_round, new_round);
}

void TestProtected(TestResults& results) {
    DiskProgram p;
    BuildDiskProgram(results, CycleProgram(100), DiskReadLayout(0x6000), p);
    if (!p.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    RunBoot(*a);
    tisple::DiskDrive drive = a->slots.disk2.drives[0];
    drive.write_protected = true;
    std::unique_ptr<tisple::Apple2> b = BootDrive(drive, p.info.return_pc);
    Boot after = RunBoot(*b);
    CHECK(results, after.finished && after.got == kOk && after.round == 1 && after.wrote == 6);
    CHECK(results, SameTracksBelow(drive, b->slots.disk2.drives[0], tisple::kDiskTracks));
}

// the writer refuses records naming a track below the save areas, the catalog's, or past 34;
// nothing on the disk changes
void TestRefused(TestResults& results) {
    const char* text = "(import save)\n"
                       "(save.area GAME 100)\n"
                       "(let fake (array u8 4))\n"
                       "(let out (array u8 4) :address 0x0300)\n"
                       "(fn try ((track u8)) u8\n"
                       "    (set (at fake 0) 0)\n"
                       "    (set (at fake 1) 1)\n"
                       "    (set (at fake 2) track)\n"
                       "    (set (at fake 3) 0)\n"
                       "    (save.write_part (addr_of (at fake 0)) (addr_of (at fake 0))))\n"
                       "(fn main () void\n"
                       "    (set (at out 0) (try 0))\n"
                       "    (set (at out 1) (try 1))\n"
                       "    (set (at out 2) (try 17))\n"
                       "    (set (at out 3) (try 35)))\n";
    DiskProgram p;
    BuildDiskProgram(results, text, DiskReadLayout(0x6000), p);
    if (!p.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    tisple::DiskDrive before = a->slots.disk2.drives[0];
    RunDiskProgram(*a, kDiskReadBudget);
    const uint8_t* out = a->cpu.mem + 0x0300;
    CHECK(results, out[0] == 7 && out[1] == 7 && out[2] == 7 && out[3] == 7);
    CHECK(results, SameTracksBelow(before, a->slots.disk2.drives[0], tisple::kDiskTracks));
}

// without a disk build there is no writer and no reader: NO_CARD
void TestNoDisk(TestResults& results) {
    LibBuild b;
    if (!BuildLibProgram(results, CycleProgram(100), tisple::BuildOptions(), b)) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = LoadLibProgram(b);
    RunToReturn(results, *a, 20000000);
    CHECK(results, a->cpu.mem[0x0300] == 2 && a->cpu.mem[0x0303] == 2);
}

} // namespace

void RunSaveLibTests(TestResults& results) {
    std::vector<uint8_t> writer;
    std::string error;
    CHECK(results, tisple::BuildDiskWriter(0x0E00, 30, 0x1700, writer, error));
    std::printf("save: the writer is %zu bytes\n", writer.size());
    for (int bytes : {1, 256, 300, 1000, 4096}) {
        TestRounds(results, bytes);
    }
    TestImage(results);
    TestPowerCut(results);
    TestProtected(results);
    TestRefused(results);
    TestNoDisk(results);
}

} // namespace tisple_tests
