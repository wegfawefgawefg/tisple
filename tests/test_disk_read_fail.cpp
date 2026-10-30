// lib/disk when things go wrong: no reader (a build without a disk), no card in the slot, no
// disk in the drive, a sector that never reads, a track whose address fields name another;
// and a head that is not where the reader thinks, which it finds again. Each read gives its
// error, never hangs, and leaves the motor off

#include "disk/disk_reader.hpp"
#include "disk/nibble.hpp"
#include "machine/disk2.hpp"
#include "test_disk.hpp"
#include "test_disk_read.hpp"

#include <cstdio>
#include <cstring>

namespace tisple_tests {

namespace {

constexpr uint16_t kStatus = 0x03C0;
constexpr uint16_t kReader = 0x1700; // lib/disk's disk.reader
constexpr uint32_t kSize = 3000;

std::string FailProgram(const std::vector<uint8_t>& table) {
    return "(import disk)\n"
           "(const T (array u8 3000) " +
           MakeBytes(table) +
           " :on disk)\n"
           "(let status u8 0xEE :address 0x03C0)\n"
           "(let room (array u8 3000) :address 0x2000 :reserve)\n"
           "(fn main () void\n"
           "    (set status (disk.read (as (ptr u8) 0x2000) (addr_of (at T 0)) disk.MAIN)))\n";
}

// what to do to the machine once the program starts (the loader has written the slot)
enum class Upset : uint8_t {
    kNone,
    kSlot5,    // the reader's slot to 5, where there is no card
    kEject,    // the disk out of the drive
    kLostHead, // the reader thinks the head is on track 3
};

struct AtEntry {
    tisple::Apple2* a = nullptr;
    uint16_t entry = 0;
    Upset upset = Upset::kNone;
    bool done = false;
};

void UpsetAtEntry(void* ctx, const tisple::Machine& m) {
    AtEntry& e = *static_cast<AtEntry*>(ctx);
    if (e.done || tisple::GetRegs(m).pc != e.entry) {
        return;
    }
    e.done = true;
    if (e.upset == Upset::kSlot5) {
        e.a->cpu.mem[kReader + tisple::kReaderSlot] = 0x50;
    } else if (e.upset == Upset::kEject) {
        tisple::EjectDisk(*e.a, 0);
    } else if (e.upset == Upset::kLostHead) {
        e.a->cpu.mem[kReader + tisple::kReaderSlot + 1] = 6; // half-track 6: track 3
    }
}

struct Outcome {
    uint8_t status = 0;
    bool returned = false;
    bool motor_off = false;
    uint64_t cycles = 0;
    bool bytes = false;
};

Outcome Run(const DiskProgram& p, const std::vector<uint8_t>& table, Upset upset,
            const tisple::DiskTrack* replace, int replace_track) {
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    if (replace != nullptr) {
        a->slots.disk2.drives[0].tracks[replace_track] = *replace;
    }
    AtEntry at;
    at.a = a.get();
    at.entry = p.code.origin;
    at.upset = upset;
    uint64_t start = tisple::Cycles(*a);
    while (!a->stopped && tisple::Cycles(*a) - start < kDiskReadBudget) {
        tisple::RunApple2Traced(*a, tisple::kCyclesPerFrame, UpsetAtEntry, &at);
    }
    Outcome out;
    out.cycles = tisple::Cycles(*a) - start;
    out.returned = a->stop_reason == tisple::MachineStop::kReturned;
    out.status = a->cpu.mem[kStatus];
    out.motor_off = !tisple::DriveSpinning(*a, tisple::Cycles(*a) + tisple::kMotorOffDelay);
    out.bytes = std::memcmp(a->cpu.mem + 0x2000, table.data(), table.size()) == 0;
    return out;
}

// the track's raw disk bytes with physical sector p's data field broken (a nibble changed, so
// the checksum fails), as a .nib-style track
tisple::DiskTrack BrokenSector(const tisple::DiskTrack& track, int physical) {
    std::vector<uint8_t> nibbles(8000);
    uint32_t count = tisple::TrackNibbles(track, nibbles.data(), 7000);
    for (uint32_t i = 0; i + 20 < count; i++) {
        bool address = nibbles[i] == 0xD5 && nibbles[i + 1] == 0xAA && nibbles[i + 2] == 0x96;
        if (!address || tisple::Decode44(nibbles[i + 7], nibbles[i + 8]) != physical) {
            continue;
        }
        for (uint32_t j = i + 14; j + 100 < count; j++) {
            if (nibbles[j] == 0xD5 && nibbles[j + 1] == 0xAA && nibbles[j + 2] == 0xAD) {
                uint8_t& n = nibbles[j + 3 + 90];
                n = n == 0x96 ? 0x97 : 0x96;
                break;
            }
        }
        break;
    }
    tisple::DiskTrack out;
    tisple::TrackFromNibbles(nibbles.data(), count, out);
    return out;
}

} // namespace

void RunDiskReadFailTests(TestResults& results) {
    std::vector<uint8_t> table = DiskPattern(kSize, 99);
    DiskProgram p;
    BuildDiskProgram(results, FailProgram(table), DiskReadLayout(0x6000), p);
    if (!p.ok) {
        return;
    }
    const tisple::DiskTable& t = p.program.disk_tables[0];
    // as it should go
    Outcome good = Run(p, table, Upset::kNone, nullptr, 0);
    CHECK(results, good.returned && good.status == tisple::kReaderDone && good.bytes);
    CHECK(results, good.motor_off);
    // the head somewhere else than the reader thinks: an address field puts it right
    Outcome lost = Run(p, table, Upset::kLostHead, nullptr, 0);
    CHECK(results, lost.returned && lost.status == tisple::kReaderDone && lost.bytes);
    // no card in the slot
    Outcome slot = Run(p, table, Upset::kSlot5, nullptr, 0);
    CHECK(results, slot.returned && slot.status == tisple::kReaderNoCard);
    // no disk: the drive reads noise; two turns without an address field, from track 0 too
    Outcome eject = Run(p, table, Upset::kEject, nullptr, 0);
    CHECK(results, eject.returned && eject.status == tisple::kReaderNoDisk && eject.motor_off);
    CHECK(results, eject.cycles < 3 * 1023000);
    // a sector of the table that never reads
    tisple::DiskTrack broken;
    {
        std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
        broken = BrokenSector(a->slots.disk2.drives[0].tracks[t.tracks[5]], t.sectors[5]);
    }
    Outcome bad = Run(p, table, Upset::kNone, &broken, t.tracks[5]);
    CHECK(results, bad.returned && bad.status == tisple::kReaderBadSector && bad.motor_off);
    // the table's track holds another one's sectors: its address fields say so
    tisple::DiskTrack other;
    {
        std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
        other = a->slots.disk2.drives[0].tracks[t.tracks[0] - 1];
    }
    Outcome wrong = Run(p, table, Upset::kNone, &other, t.tracks[0]);
    CHECK(results, wrong.returned && wrong.status == tisple::kReaderBadTrack && wrong.motor_off);
    // without a disk build there is no reader at all: NO_CARD, nothing touched
    std::unique_ptr<tisple::Apple2> direct = tisple::NewApple2();
    std::string error;
    for (const tisple::MemorySegment& s : p.code.segments) {
        tisple::LoadSegment(*direct, s, error);
    }
    tisple::LoadProgram(*direct, p.code.image.data(), p.code.image.size(), p.code.origin, error);
    tisple::RunApple2(*direct, 1000000);
    CHECK(results, direct->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, direct->cpu.mem[kStatus] == tisple::kReaderNoCard);
    std::printf("disk read errors: no disk in %.2f s, a bad sector in %.2f s, a wrong track in "
                "%.2f s\n",
                static_cast<double>(eject.cycles) / 1023000.0,
                static_cast<double>(bad.cycles) / 1023000.0,
                static_cast<double>(wrong.cycles) / 1023000.0);
}

} // namespace tisple_tests
