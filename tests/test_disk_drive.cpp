// the disk ii card: byte timing, reads and writes from 6502 code, the stepper, two drives

#include "disk/code6502.hpp"
#include "disk/machine_disks.hpp"
#include "machine/disk2.hpp"
#include "test_disk.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unistd.h>

namespace tisple_tests {

namespace {

// one card access at a given cycle, as the cpu would make it
uint8_t Access(tisple::Apple2& a, int reg, uint64_t cycle, bool is_write = false,
               uint8_t value = 0) {
    a.cpu.cycles = cycle + 1; // the bus counts a cycle before performing it
    bool drives = false;
    return tisple::Disk2IoAccess(a, reg, is_write, value, drives);
}

// cells each disk byte takes from cell 0 on: 8, plus the zeros before it
std::vector<uint32_t> NibbleCells(const tisple::DiskTrack& t) {
    std::vector<uint32_t> cells;
    uint32_t zeros = 0;
    uint32_t got = 0;
    for (uint32_t pos = 0; pos < t.bit_count; pos++) {
        bool one = (t.bits[pos >> 3] & (0x80 >> (pos & 7))) != 0;
        if (got == 0 && !one) {
            zeros++;
            continue;
        }
        got++;
        if (got == 8) {
            cells.push_back(zeros + 8);
            zeros = 0;
            got = 0;
        }
    }
    return cells;
}

// the latch polled every cycle: each byte once, when its top bit comes on
std::vector<uint8_t> PolledBytes(tisple::Apple2& a, uint64_t from, uint64_t to) {
    std::vector<uint8_t> out;
    uint8_t before = 0;
    for (uint64_t t = from; t < to; t++) {
        uint8_t v = Access(a, 0xC, t);
        if ((v & 0x80) != 0 && (before & 0x80) == 0) {
            out.push_back(v);
        }
        before = v;
    }
    return out;
}

// whether count bytes of the track, from somewhere, appear in a row
bool HasRun(const std::vector<uint8_t>& seen, const tisple::DiskTrack& track, size_t count) {
    std::vector<uint8_t> expect(tisple::kTrackBytes);
    uint32_t n = tisple::TrackNibbles(track, expect.data(), tisple::kTrackBytes);
    expect.resize(n);
    expect.insert(expect.end(), expect.begin(), expect.end());
    for (size_t at = 0; at + count <= expect.size(); at++) {
        for (size_t from = 0; from + count <= seen.size() && from < 64; from++) {
            if (std::memcmp(expect.data() + at, seen.data() + from, count) == 0) {
                return true;
            }
        }
    }
    return false;
}

// poll the latch every cycle for a turn: each byte shows up 32 cycles after the one before,
// 40 after a self-sync byte, holds at least 8 cycles, and the turn takes 4 cycles a cell
void TestByteTiming(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> disk = NoiseDisk(11);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*disk);
    const tisple::DiskTrack& track = a->slots.disk2.drives[0].tracks[0];
    uint64_t start = 1000;
    Access(*a, 0x9, start); // motor on: cell 0 starts here
    Access(*a, 0xE, start);
    uint64_t turn = static_cast<uint64_t>(track.bit_count) * tisple::kCyclesPerBitCell;
    std::vector<uint8_t> values;
    std::vector<uint64_t> times;
    std::vector<uint32_t> holds;
    uint8_t before = 0;
    for (uint64_t t = start + 1; t < start + 2 * turn + 100; t++) {
        uint8_t v = Access(*a, 0xC, t);
        if ((v & 0x80) != 0 && (before & 0x80) == 0) {
            values.push_back(v);
            times.push_back(t);
            holds.push_back(0);
        }
        if ((v & 0x80) != 0 && !holds.empty()) {
            holds.back()++;
        }
        before = v;
    }
    // the second turn, byte by byte, is the track as an ideal reader sees it
    std::vector<uint8_t> expect(tisple::kTrackBytes);
    uint32_t count = tisple::TrackNibbles(track, expect.data(), tisple::kTrackBytes);
    expect.resize(count);
    std::vector<uint32_t> cells = NibbleCells(track);
    CHECK(results, cells.size() == count);
    size_t first = 0;
    while (first < times.size() && times[first] < start + turn) {
        first++;
    }
    bool same = first + count <= values.size();
    bool spacing = same;
    for (size_t i = 0; same && i < count; i++) {
        same = values[first + i] == expect[i];
        if (i > 0) {
            spacing = spacing && times[first + i] - times[first + i - 1] == 4 * cells[i];
        }
    }
    CHECK(results, same);
    CHECK(results, spacing);
    CHECK(results, times[first + count] - times[first] == turn);
    bool held = true;
    for (size_t i = first; i < first + count; i++) {
        held = held && holds[i] >= 8;
    }
    CHECK(results, held);
    // 16 sectors of 32-cycle bytes and 40-cycle syncs: a turn is just over 200 ms
    CHECK(results, turn == 205056);

    // motor off: the disk turns on for a second, then the latch holds still
    Access(*a, 0x8, start + 3 * turn);
    uint64_t stop = start + 3 * turn + tisple::kMotorOffDelay;
    CHECK(results, tisple::DriveSpinning(*a, stop - 1) && !tisple::DriveSpinning(*a, stop));
    uint8_t last = Access(*a, 0xC, stop + 10);
    bool still = true;
    for (uint64_t t = stop + 11; t < stop + 500; t++) {
        still = still && Access(*a, 0xC, t) == last;
    }
    CHECK(results, still);
}

std::unique_ptr<tisple::Apple2> MachineWithCode(const tisple::DiskSectors& disk,
                                                tisple::Code6502& code) {
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(disk);
    std::vector<uint8_t> bytes;
    std::string error;
    if (!tisple::BuildCode(code, 0x6000, bytes, error)) {
        std::fprintf(stderr, "test program: %s\n", error.c_str());
    }
    tisple::LoadProgram(*a, bytes.data(), bytes.size(), 0x6000, error);
    return a;
}

// LDA $C0EC / BPL / STA: the usual read loop gets an unbroken run of the track's bytes
void TestCpuReadLoop(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> disk = NoiseDisk(5);
    tisple::Code6502 c;
    std::int32_t loop = c.Label("loop");
    c.Imm(tisple::Mn::LDX, 0x60);
    c.MemX(tisple::Mn::LDA, 0xC089);
    c.MemX(tisple::Mn::LDA, 0xC08E);
    c.Imm(tisple::Mn::LDY, 0);
    c.Place(loop);
    c.MemX(tisple::Mn::LDA, 0xC08C);
    c.Br(tisple::Mn::BPL, loop);
    c.MemY(tisple::Mn::STA, 0x1000);
    c.Op(tisple::Mn::INY);
    c.Br(tisple::Mn::BNE, loop);
    c.Op(tisple::Mn::RTS);
    std::unique_ptr<tisple::Apple2> a = MachineWithCode(*disk, c);
    tisple::RunApple2(*a, 50000);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    const tisple::DiskTrack& track = a->slots.disk2.drives[0].tracks[0];
    std::vector<uint8_t> expect(tisple::kTrackBytes);
    uint32_t count = tisple::TrackNibbles(track, expect.data(), tisple::kTrackBytes);
    expect.resize(count);
    expect.insert(expect.end(), expect.begin(), expect.end());
    // the loop starts mid-byte, so skip the first; the rest must be a run of the track
    bool found = false;
    for (size_t at = 0; at + 255 <= expect.size() && !found; at++) {
        found = std::memcmp(expect.data() + at, a->cpu.mem + 0x1001, 255) == 0;
    }
    CHECK(results, found);
}

void Pad(tisple::Code6502& c, int cycles) {
    if (cycles % 2 != 0) {
        c.Mem(tisple::Mn::BIT, 0x00); // 3 cycles
        cycles -= 3;
    }
    for (int i = 0; i < cycles / 2; i++) {
        c.Op(tisple::Mn::NOP);
    }
}

// one disk byte written: load it, shift, and wait out the rest of its time
void WriteByte(tisple::Code6502& c, uint8_t value, int cycles) {
    c.Imm(tisple::Mn::LDA, value);
    c.MemX(tisple::Mn::STA, 0xC08D);
    c.MemX(tisple::Mn::ORA, 0xC08C);
    Pad(c, cycles - 11);
}

// find sector 3's address field on track 0, then write a new data field over the old one the
// way DOS does: five self-sync bytes (40 cycles each), then 32-cycle bytes
tisple::Code6502 SectorWriter(const uint8_t data[256]) {
    using tisple::Mn;
    tisple::Code6502 c;
    std::int32_t f1 = c.Label("f1");
    std::int32_t f2 = c.Label("f2");
    std::int32_t f3 = c.Label("f3");
    std::int32_t skip = c.Label("skip");
    std::int32_t odd = c.Label("odd");
    std::int32_t even = c.Label("even");
    std::int32_t tail = c.Label("tail");
    std::int32_t fail = c.Label("fail");
    c.Imm(Mn::LDX, 0x60);
    c.MemX(Mn::LDA, 0xC089);
    c.MemX(Mn::LDA, 0xC08E);
    c.Place(f1);
    c.MemX(Mn::LDA, 0xC08C);
    c.Br(Mn::BPL, f1);
    c.Imm(Mn::CMP, 0xD5);
    c.Br(Mn::BNE, f1);
    c.Place(f2);
    c.MemX(Mn::LDA, 0xC08C);
    c.Br(Mn::BPL, f2);
    c.Imm(Mn::CMP, 0xAA);
    c.Br(Mn::BNE, f1);
    c.Place(f3);
    c.MemX(Mn::LDA, 0xC08C);
    c.Br(Mn::BPL, f3);
    c.Imm(Mn::CMP, 0x96);
    c.Br(Mn::BNE, f1);
    c.Imm(Mn::LDY, 4); // volume and track
    c.Place(skip);
    c.MemX(Mn::LDA, 0xC08C);
    c.Br(Mn::BPL, skip);
    c.Op(Mn::DEY);
    c.Br(Mn::BNE, skip);
    c.Place(odd);
    c.MemX(Mn::LDA, 0xC08C);
    c.Br(Mn::BPL, odd);
    c.Op(Mn::SEC);
    c.OpA(Mn::ROL);
    c.Mem(Mn::STA, 0x00);
    c.Place(even);
    c.MemX(Mn::LDA, 0xC08C);
    c.Br(Mn::BPL, even);
    c.Mem(Mn::AND, 0x00);
    c.Imm(Mn::CMP, 3);
    c.Br(Mn::BNE, f1);
    c.Imm(Mn::LDY, 4); // checksum, DE AA
    c.Place(tail);
    c.MemX(Mn::LDA, 0xC08C);
    c.Br(Mn::BPL, tail);
    c.Op(Mn::DEY);
    c.Br(Mn::BNE, tail);
    c.MemX(Mn::LDA, 0xC08D); // sense write protect
    c.MemX(Mn::LDA, 0xC08E);
    std::int32_t writable = c.Label("writable");
    c.Br(Mn::BPL, writable);
    c.Jump(Mn::JMP, fail); // the writer is too long to branch over
    c.Place(writable);
    c.Imm(Mn::LDA, 0xFF);
    c.MemX(Mn::STA, 0xC08F); // write mode, loading the first sync byte
    c.MemX(Mn::ORA, 0xC08C);
    Pad(c, 40 - 11);
    for (int i = 0; i < 4; i++) {
        WriteByte(c, 0xFF, 40);
    }
    uint8_t nibbles[tisple::kEncodedSectorNibbles];
    tisple::Encode62(data, nibbles);
    const uint8_t prologue[] = {0xD5, 0xAA, 0xAD};
    const uint8_t epilogue[] = {0xDE, 0xAA, 0xEB, 0xFF};
    for (uint8_t b : prologue) {
        WriteByte(c, b, 32);
    }
    for (uint8_t b : nibbles) {
        WriteByte(c, b, 32);
    }
    for (uint8_t b : epilogue) {
        WriteByte(c, b, 32);
    }
    Pad(c, 32);
    c.MemX(Mn::LDA, 0xC08E); // back to reading
    c.MemX(Mn::LDA, 0xC08C);
    c.Op(Mn::RTS);
    c.Place(fail);
    c.Imm(Mn::LDA, 0xEE);
    c.Mem(Mn::STA, 0x01);
    c.MemX(Mn::LDA, 0xC08C);
    c.Op(Mn::RTS);
    return c;
}

void TestCpuWrite(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> disk = NoiseDisk(9);
    uint8_t data[256];
    for (int i = 0; i < 256; i++) {
        data[i] = static_cast<uint8_t>(255 - i);
    }
    tisple::Code6502 c = SectorWriter(data);
    std::unique_ptr<tisple::Apple2> a = MachineWithCode(*disk, c);
    tisple::RunApple2(*a, 1000000);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, a->cpu.mem[0x01] == 0x00);
    CHECK(results, a->slots.disk2.drives[0].dirty);
    // the image saved back has the new sector 3 and everything else as it was
    std::vector<uint8_t> saved;
    std::string error;
    CHECK(results, tisple::SaveDiskImage(*a, 0, saved, error));
    std::memcpy(disk->data[0][3], data, 256);
    CHECK(results, saved == tisple::ImageFromSectors(*disk, tisple::SectorOrder::kDos));
    // --write-back: the written drive goes to its file, in its own format
    tisple::MachineDisks files;
    files.paths[0] =
        (std::filesystem::temp_directory_path() / ("tisple_wb_" + std::to_string(getpid()) + ".po"))
            .string();
    a->slots.disk2.drives[0].format = tisple::DiskFormat::kProdos;
    std::string report;
    CHECK(results, tisple::WriteBackDisks(*a, files, report));
    std::vector<uint8_t> written;
    CHECK(results, tisple::ReadWholeFile(files.paths[0], written));
    CHECK(results, written == tisple::ImageFromSectors(*disk, tisple::SectorOrder::kProdos));
    std::filesystem::remove(files.paths[0]);

    // a protected disk: the write protect sense says so and nothing changes
    std::unique_ptr<tisple::DiskSectors> other = NoiseDisk(10);
    tisple::Code6502 c2 = SectorWriter(data);
    std::unique_ptr<tisple::Apple2> b = MachineWithCode(*other, c2);
    b->slots.disk2.drives[0].write_protected = true;
    tisple::RunApple2(*b, 1000000);
    CHECK(results, b->cpu.mem[0x01] == 0xEE);
    CHECK(results, !b->slots.disk2.drives[0].dirty);
    CHECK(results, tisple::SaveDiskImage(*b, 0, saved, error));
    CHECK(results, saved == tisple::ImageFromSectors(*other, tisple::SectorOrder::kDos));
}

// phases one after another walk the head; motor off stops it
void TestStepper(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> disk = NoiseDisk(1);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*disk);
    uint64_t t = 100;
    Access(*a, 0x1, t); // phase 0 on with the motor off: no power, no move
    CHECK(results, a->slots.disk2.magnets == 0);
    Access(*a, 0x9, t);
    // up 10 half-tracks: phase n+1 on, phase n off
    for (int h = 0; h < 10; h++) {
        Access(*a, ((h + 1) & 3) * 2 + 1, t += 1000);
        Access(*a, (h & 3) * 2, t += 1000);
    }
    CHECK(results, a->slots.disk2.drives[0].half_track == 10 && tisple::DriveTrack(*a, 0) == 5);
    // a phase opposite the head does nothing; two neighbours at once pull both ways
    Access(*a, 0x1, t += 1000); // phase 0: opposite half-track 10's phase 2
    CHECK(results, a->slots.disk2.drives[0].half_track == 10);
    Access(*a, 0x0, t += 1000);
    // down to track 0, and one more step: the head stops there
    for (int h = 10; h > 0; h--) {
        Access(*a, ((h + 3) & 3) * 2 + 1, t += 1000);
        Access(*a, (h & 3) * 2, t += 1000);
    }
    CHECK(results, a->slots.disk2.drives[0].half_track == 0);
    Access(*a, 0x7, t += 1000);
    Access(*a, 0x6, t += 1000);
    CHECK(results, a->slots.disk2.drives[0].half_track == 0);
    Access(*a, 0x3, t += 1000); // phase 1 on
    Access(*a, 0x8, t += 1000); // motor off clears the magnets
    CHECK(results, a->slots.disk2.magnets == 0 && a->slots.disk2.drives[0].half_track == 1);

    // the track under the head is the one read: step to track 1, see its bytes
    std::unique_ptr<tisple::Apple2> b = MachineWithDisk(*disk);
    t = 100;
    Access(*b, 0x9, t);
    for (int h = 0; h < 2; h++) {
        Access(*b, ((h + 1) & 3) * 2 + 1, t += 1000);
        Access(*b, (h & 3) * 2, t += 1000);
    }
    const tisple::DiskTrack& track1 = b->slots.disk2.drives[0].tracks[1];
    std::vector<uint8_t> seen = PolledBytes(*b, t, t + 2 * track1.bit_count * 4);
    CHECK(results, HasRun(seen, track1, 400));
}

// drive 2 is its own disk; the drive left behind stops turning
void TestTwoDrives(TestResults& results) {
    std::unique_ptr<tisple::DiskSectors> one = NoiseDisk(21);
    std::unique_ptr<tisple::DiskSectors> two = NoiseDisk(22);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*one);
    std::string error;
    CHECK(results,
          tisple::InsertDiskImage(*a, 1, tisple::ImageFromSectors(*two, tisple::SectorOrder::kDos),
                                  tisple::DiskFormat::kDos, false, error));
    uint64_t t = 100;
    Access(*a, 0x9, t);
    Access(*a, 0xC, t += 5000);
    Access(*a, 0xB, t += 4); // drive 2
    uint32_t pos1 = a->slots.disk2.drives[0].bit_pos;
    for (int i = 0; i < 1000; i++) {
        Access(*a, 0xC, t += 4);
    }
    CHECK(results, a->slots.disk2.drives[0].bit_pos == pos1);
    CHECK(results, a->slots.disk2.drives[1].bit_pos == 1000);
    std::vector<uint8_t> seen = PolledBytes(*a, t, t + 300000);
    CHECK(results, HasRun(seen, a->slots.disk2.drives[1].tracks[0], 400));
    CHECK(results, !HasRun(seen, a->slots.disk2.drives[0].tracks[0], 400));
    CHECK(results, tisple::HasDisk(*a, 1));
    std::vector<uint8_t> saved;
    CHECK(results, tisple::SaveDiskImage(*a, 1, saved, error));
    CHECK(results, saved == tisple::ImageFromSectors(*two, tisple::SectorOrder::kDos));
    tisple::EjectDisk(*a, 1);
    CHECK(results, !tisple::HasDisk(*a, 1) && !tisple::SaveDiskImage(*a, 1, saved, error));
    // reset selects drive 1 and stops the motor
    tisple::ResetApple2(*a);
    CHECK(results, a->slots.disk2.drive == 0 && !a->slots.disk2.motor_on);
}

} // namespace

void RunDiskDriveTests(TestResults& results) {
    TestByteTiming(results);
    TestCpuReadLoop(results);
    TestCpuWrite(results);
    TestStepper(results);
    TestTwoDrives(results);
}

} // namespace tisple_tests
