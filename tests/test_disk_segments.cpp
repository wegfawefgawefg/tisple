// the loader with a program in pieces: main memory below and above, the language card and
// aux memory, past track 17; and a compiled multi-region program booting like a direct run

#include "disk/boot_loader.hpp"
#include "disk/dos33.hpp"
#include "machine/disk2.hpp"
#include "test_backend.hpp"
#include "test_disk.hpp"

#include <cstdio>
#include <cstring>

namespace tisple_tests {

namespace {

using tisple::MemoryBank;
using tisple::MemorySegment;

constexpr uint64_t kBudget = 200ull * 1000 * 1000;

uint64_t BootToStop(tisple::Apple2& a, uint16_t return_pc) {
    a.return_pc = return_pc;
    tisple::StartDiskBoot(a);
    uint64_t start = tisple::Cycles(a);
    while (!a.stopped && tisple::Cycles(a) - start < kBudget) {
        tisple::RunApple2(a, tisple::kCyclesPerFrame);
    }
    return tisple::Cycles(a) - start;
}

MemorySegment Noise(MemoryBank bank, uint16_t address, size_t size, uint32_t seed) {
    MemorySegment s;
    s.bank = bank;
    s.address = address;
    s.bytes.resize(size);
    for (uint8_t& b : s.bytes) {
        seed = seed * 1103515245 + 12345;
        b = static_cast<uint8_t>(seed >> 16);
    }
    return s;
}

// the bytes of a segment as they are in the machine's memory now
bool Loaded(const tisple::Apple2& a, const MemorySegment& s) {
    for (size_t i = 0; i < s.bytes.size(); i++) {
        uint32_t at = s.address + static_cast<uint32_t>(i);
        uint8_t got = 0;
        if (s.bank == MemoryBank::kMain) {
            got = a.cpu.mem[at];
        } else if (s.bank == MemoryBank::kAux) {
            got = a.aux[at];
        } else {
            got = at >= 0xE000 ? a.lc.e000[at - 0xE000] : a.lc.bank2_d000[at - 0xD000];
        }
        if (got != s.bytes[i]) {
            std::fprintf(stderr, "%s byte at $%04X: %02X, want %02X\n",
                         tisple::MemoryBankName(s.bank), at, got, s.bytes[i]);
            return false;
        }
    }
    return true;
}

// 17 tracks of segments in every bank: the catalog's track is skipped and still reads
void TestSegmentsBoot(TestResults& results) {
    std::vector<MemorySegment> segments;
    segments.push_back(Noise(MemoryBank::kMain, 0x6000, 0x0800, 1));
    segments[0].bytes[0] = 0x60; // RTS at the entry
    segments.push_back(Noise(MemoryBank::kMain, 0x0B20, 0x04C0, 2));
    segments.push_back(Noise(MemoryBank::kCard, 0xD000, 0x3000, 3));
    segments.push_back(Noise(MemoryBank::kAux, 0x0800, 0xB800, 4));
    auto disk = std::make_unique<tisple::DiskSectors>();
    tisple::BootDiskInfo info;
    std::string error;
    CHECK(results, tisple::MakeBootDisk(segments, 0x6000, *disk, info, error));
    CHECK(results, info.segments == 4 && info.last_track == 18);
    // tracks 0-16 and 18 are the program's, 17 the catalog's
    CHECK(results, tisple::DosFreeSectors(*disk) == (35 - 19) * 16);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*disk);
    uint64_t took = BootToStop(*a, info.return_pc);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    for (const MemorySegment& s : segments) {
        CHECK(results, Loaded(*a, s));
    }
    // around the segments the pages hold zeros; the loader leaves the card on rom reads
    CHECK(results, a->cpu.mem[0x0B1F] == 0 && a->cpu.mem[0x6800] == 0);
    CHECK(results, !a->lc.read_ram && !a->lc.write_enable);
    CHECK(results, !a->sw.ramwrt && !a->sw.ramrd);
    // two or three turns a track (an aux sector's copy can miss the next one), 18 tracks:
    // about 10 seconds
    if (took >= 12ull * 1023000) {
        std::fprintf(stderr, "segments boot took %llu cycles\n",
                     static_cast<unsigned long long>(took));
    }
    CHECK(results, took < 12ull * 1023000);

    // what does not fit, or loads where it cannot, is refused
    std::vector<MemorySegment> low = {Noise(MemoryBank::kMain, 0x0A00, 16, 5)};
    CHECK(results, !tisple::MakeBootDisk(low, 0x0A00, *disk, info, error));
    std::vector<MemorySegment> card = {Noise(MemoryBank::kCard, 0xC800, 16, 5)};
    CHECK(results, !tisple::MakeBootDisk(card, 0x6000, *disk, info, error));
    std::vector<MemorySegment> full = segments;
    full.push_back(Noise(MemoryBank::kAux, 0x0800, 0xB800, 6));
    full.push_back(Noise(MemoryBank::kAux, 0x0800, 0xB800, 7));
    full.push_back(Noise(MemoryBank::kAux, 0x0800, 0xB800, 8));
    CHECK(results, !tisple::MakeBootDisk(full, 0x6000, *disk, info, error));
    CHECK(results, error.find("528") != std::string::npos);
}

const char* const kRegions = R"(
; data in every region a disk can load: after the code, below the origin, in the language
; card and in aux memory
(let a (array u8 20000))
(let b (array u8 20000))
(macro counting (n)
    `(make (array u8 ,n) ,@(map (fn (i) (% i 249)) (range 0 n))))
(let c (array u8 9000) (counting 9000))
(let d (array u8 3000) (counting 3000))
(let doc (array u8 4000) (counting 4000) :in aux)
(let sum u16 0 :address 0x0300)

(fn main () void
    (set (at a 19999) 1)
    (set (at b 19999) 2)
    (set sum (+ (+ (as u16 (at a 19999)) (as u16 (at b 19999)))
        (+ (as u16 (at c 8999)) (as u16 (at d 2999))))))
)";

// a compiled program in four regions boots and runs as it does loaded straight in
void TestCompiledRegionsBoot(TestResults& results) {
    // checked only: the test layout's one region could not hold it
    Built built;
    tisple::LoadOptions load;
    load.files.push_back(tisple::VirtualFile{"t.tsp", kRegions});
    built.ok = tisple::BuildProgram("t.tsp", load, built.program, built.errors);
    CHECK(results, built.ok);
    if (!built.ok) {
        std::fprintf(stderr, "%s", ErrorText(built).c_str());
        return;
    }
    tisple::BuildOptions options; // as tisple build --disk: no DOS, the loader below $0B00
    options.limit = 0xC000;
    options.low_start = tisple::kBootLoadLow;
    tisple::Compiled code;
    std::vector<tisple::Diagnostic> errors;
    bool ok = tisple::CompileProgram(built.program, options, code, errors);
    CHECK(results, ok);
    if (!ok) {
        std::fprintf(stderr, "%s",
                     tisple::FormatDiagnostics(built.program.sources, errors).c_str());
        return;
    }
    std::vector<MemorySegment> segments = tisple::ProgramSegments(code);
    bool card = false;
    bool aux = false;
    bool low = false;
    for (const MemorySegment& s : segments) {
        card = card || s.bank == MemoryBank::kCard;
        aux = aux || s.bank == MemoryBank::kAux;
        low = low || (s.bank == MemoryBank::kMain && s.address < code.origin);
    }
    CHECK(results, card && aux && low && code.card_ram);

    auto disk = std::make_unique<tisple::DiskSectors>();
    tisple::BootDiskInfo info;
    std::string error;
    CHECK(results, tisple::MakeBootDisk(segments, code.origin, *disk, info, error));
    std::unique_ptr<tisple::Apple2> booted = MachineWithDisk(*disk);
    BootToStop(*booted, info.return_pc);
    CHECK(results, booted->stop_reason == tisple::MachineStop::kReturned);
    // and through Apple's main and disk roms, when the owner has them
    std::vector<uint8_t> main_rom = OwnerRom("Apple2e.rom");
    std::vector<uint8_t> disk_rom = OwnerRom("DISK2.rom");
    std::unique_ptr<tisple::Apple2> apple;
    if (!main_rom.empty() && disk_rom.size() == 256) {
        apple = MachineWithDisk(*disk);
        CHECK(results, tisple::LoadRom(*apple, main_rom.data(), main_rom.size(), error));
        std::memcpy(apple->slots.disk2.rom, disk_rom.data(), 256);
        apple->return_pc = info.return_pc;
        tisple::ResetApple2(*apple);
        uint64_t start = tisple::Cycles(*apple);
        while (!apple->stopped && tisple::Cycles(*apple) - start < kBudget) {
            tisple::RunApple2(*apple, tisple::kCyclesPerFrame);
        }
        CHECK(results, apple->stop_reason == tisple::MachineStop::kReturned);
        CHECK(results, std::memcmp(apple->cpu.mem + 0x0300, booted->cpu.mem + 0x0300, 2) == 0);
    } else {
        std::printf("disk: Apple ROMs not found, skipping the Apple ROM segments boot\n");
    }

    std::unique_ptr<tisple::Apple2> direct = tisple::NewApple2();
    for (size_t i = 1; i < segments.size(); i++) {
        CHECK(results, tisple::LoadSegment(*direct, segments[i], error));
    }
    CHECK(results,
          tisple::LoadProgram(*direct, code.image.data(), code.image.size(), code.origin, error));
    tisple::RunApple2(*direct, 10 * 1000 * 1000);
    CHECK(results, direct->stop_reason == tisple::MachineStop::kReturned);
    uint16_t want = 1 + 2 + 8999 % 249 + 2999 % 249;
    CHECK(results, (booted->cpu.mem[0x0300] | (booted->cpu.mem[0x0301] << 8)) == want);
    CHECK(results, std::memcmp(booted->cpu.mem + 0x0300, direct->cpu.mem + 0x0300, 2) == 0);
    for (const MemorySegment& s : segments) {
        if (s.bank == MemoryBank::kAux) {
            CHECK(results, Loaded(*booted, s));
            CHECK(results, apple == nullptr || Loaded(*apple, s));
        }
    }
}

} // namespace

void RunDiskSegmentTests(TestResults& results) {
    TestSegmentsBoot(results);
    TestCompiledRegionsBoot(results);
}

} // namespace tisple_tests
