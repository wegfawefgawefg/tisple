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
        } else if (s.bank == MemoryBank::kCardBank1) {
            got = a.lc.bank1_d000[at - 0xD000];
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
    std::vector<MemorySegment> twice = segments;
    twice.push_back(Noise(MemoryBank::kAux, 0x2000, 0x0100, 6));
    CHECK(results, !tisple::MakeBootDisk(twice, 0x6000, *disk, info, error));
    CHECK(results,
          error.find("$0800-$BFFF and $2000-$20FF in aux memory overlap") != std::string::npos);
}

// both of the card's $D000 banks: bank 2's segment and bank 1's, each where it belongs
void TestBank1Boot(TestResults& results) {
    std::vector<MemorySegment> segments;
    segments.push_back(Noise(MemoryBank::kMain, 0x6000, 0x0100, 7));
    segments[0].bytes[0] = 0x60; // RTS at the entry
    segments.push_back(Noise(MemoryBank::kCard, 0xD000, 0x1800, 8));
    segments.push_back(Noise(MemoryBank::kCardBank1, 0xD000, 0x0F00, 9));
    auto disk = std::make_unique<tisple::DiskSectors>();
    tisple::BootDiskInfo info;
    std::string error;
    CHECK(results, tisple::MakeBootDisk(segments, 0x6000, *disk, info, error));
    CHECK(results, info.segments == 3);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*disk);
    BootToStop(*a, info.return_pc);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    for (const MemorySegment& s : segments) {
        CHECK(results, Loaded(*a, s));
    }
    std::vector<MemorySegment> high = {segments[0],
                                       Noise(MemoryBank::kCardBank1, 0xDF00, 0x200, 5)};
    CHECK(results, !tisple::MakeBootDisk(high, 0x6000, *disk, info, error));
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

(fn left () u16 (+ (as u16 (at a 19999)) (as u16 (at b 19999))))
(fn right () u16 (+ (as u16 (at c 8999)) (as u16 (at d 2999))))
(fn main () void
    (set (at a 19999) 1)
    (set (at b 19999) 2)
    (set sum (+ (left) (right))))
)";

// a compiled program in four regions boots and runs as it does loaded straight in; with bank1,
// its function in the card's bank 1 too (BuildOptions::bank1_test), with overlay, its functions
// in overlay groups whose images the loader puts in aux memory (BuildOptions::overlay_test)
void TestCompiledRegionsBoot(TestResults& results, int bank1, int overlay = 0) {
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
    options.bank1_test = bank1;
    options.overlay_test = overlay;
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
    bool banked = false;
    for (const MemorySegment& s : segments) {
        banked = banked || s.bank == MemoryBank::kCardBank1;
        card = card || s.bank == MemoryBank::kCard;
        aux = aux || s.bank == MemoryBank::kAux;
        low = low || (s.bank == MemoryBank::kMain && s.address < code.origin);
    }
    CHECK(results, card && aux && low && code.card_ram);
    CHECK(results, banked == (bank1 > 0));
    CHECK(results, code.overlays.groups.empty() == (overlay == 0));

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
        if (s.bank == MemoryBank::kAux || s.bank == MemoryBank::kCardBank1) {
            CHECK(results, Loaded(*booted, s));
            CHECK(results, apple == nullptr || Loaded(*apple, s));
        }
    }
}

// text page 1 while the loader runs: spaces but for row 12, LOADING and the percent from
// column 14 ("LOADING  45%"), and row 14, the meter's 20 cells from column 10, one full (an
// inverse space) for each 5%, the rest _. Gives the percent, or -1 for anything else
int LoadingScreen(const tisple::Apple2& a) {
    if (!a.sw.text || a.sw.page2) {
        return -1;
    }
    const uint8_t* row12 = a.cpu.mem + 0x0628;
    int percent = 0;
    for (int col = 22; col < 25; col++) {
        uint8_t c = row12[col];
        if (c >= 0xB0 && c <= 0xB9) {
            percent = percent * 10 + (c - 0xB0);
        } else if (c != 0xA0 || percent != 0 || col == 24) {
            return -1;
        }
    }
    char want12[41];
    std::snprintf(want12, sizeof(want12), "%-14sLOADING %3d%%%-14s", "", percent, "");
    for (int row = 0; row < 24; row++) {
        const uint8_t* line = a.cpu.mem + 0x0400 + (row % 8) * 0x80 + (row / 8) * 40;
        for (int col = 0; col < 40; col++) {
            uint8_t want = 0xA0;
            if (row == 12) {
                want = static_cast<uint8_t>(want12[col] | 0x80);
            } else if (row == 14 && col >= 10 && col < 30) {
                want = col - 10 < percent / 5 ? 0x20 : 0xDF;
            }
            if (line[col] != want) {
                std::fprintf(stderr, "screen row %d column %d: %02X, want %02X\n", row, col,
                             line[col], want);
                return -1;
            }
        }
    }
    return percent;
}

// segments of one memory that share a page (two rows of text page 2 reserved split page $0B
// in three): each loads whole, and the rows between them are zeros. Meanwhile the screen
// shows text page 1 blank but for LOADING and the meter, aux sectors and all
void TestSharedPagesBoot(TestResults& results) {
    std::vector<MemorySegment> segments;
    segments.push_back(Noise(MemoryBank::kMain, 0x6000, 0x0300, 11));
    segments[0].bytes[0] = 0x60; // RTS at the entry
    segments.push_back(Noise(MemoryBank::kMain, 0x0B00, 0x50, 12));
    segments.push_back(Noise(MemoryBank::kMain, 0x0B78, 0x58, 13));
    segments.push_back(Noise(MemoryBank::kMain, 0x0BF8, 0x1400, 14));
    segments.push_back(Noise(MemoryBank::kAux, 0x0800, 0x1FC0, 15));
    segments.push_back(Noise(MemoryBank::kAux, 0x27E0, 0x0100, 16));
    segments.push_back(Noise(MemoryBank::kCard, 0xD000, 0x0100, 17));
    auto disk = std::make_unique<tisple::DiskSectors>();
    tisple::BootDiskInfo info;
    std::string error;
    CHECK(results, tisple::MakeBootDisk(segments, 0x6000, *disk, info, error));
    CHECK(results, info.segments == 4 && info.first_page == 0x60); // $0B with $0C-$1F, aux
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*disk);
    a->return_pc = info.return_pc;
    tisple::StartDiskBoot(*a);
    bool clean = true;
    for (int frame = 0; frame < 600 && !a->stopped; frame++) {
        tisple::RunApple2(*a, tisple::kCyclesPerFrame);
        if (frame >= 30 && frame % 30 == 0) {
            clean = clean && LoadingScreen(*a) >= 0;
        }
    }
    CHECK(results, clean);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    for (const MemorySegment& s : segments) {
        CHECK(results, Loaded(*a, s));
    }
    CHECK(results, a->cpu.mem[0x0B50] == 0 && a->cpu.mem[0x0BF7] == 0);
    CHECK(results, a->aux[0x27C0] == 0 && a->aux[0x27DF] == 0);
}

// the meter counts the pages in, a track at a time: aux 33 pages (tracks of 16, 16, 1), then
// main 20 (16, 4) and 1, so 54 in all, and after each track floor(pages * 100 / 54) percent,
// every one of them shown, the meter a cell for each 5%; 100% and every cell full at the end
void TestBootMeter(TestResults& results) {
    std::vector<MemorySegment> segments;
    segments.push_back(Noise(MemoryBank::kMain, 0x6000, 0x1301, 21)); // pages $60-$73
    segments[0].bytes[0] = 0x60;                                      // RTS at the entry
    segments.push_back(Noise(MemoryBank::kAux, 0x0800, 0x2100, 22));  // $08-$28
    segments.push_back(Noise(MemoryBank::kMain, 0x0B00, 0x0100, 23));
    auto disk = std::make_unique<tisple::DiskSectors>();
    tisple::BootDiskInfo info;
    std::string error;
    CHECK(results, tisple::MakeBootDisk(segments, 0x6000, *disk, info, error));
    CHECK(results, info.pages == 54);
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*disk);
    a->return_pc = info.return_pc;
    tisple::StartDiskBoot(*a);
    // a track's count takes under 3,000 cycles, so a screen the same 5,000 cycles on is not
    // one caught in the middle of it
    std::vector<int> shown;
    bool clean = true;
    for (int frame = 0; frame < 600 && !a->stopped; frame++) {
        tisple::RunApple2(*a, tisple::kCyclesPerFrame - 5000);
        uint8_t before[0x0400];
        std::memcpy(before, a->cpu.mem + 0x0400, sizeof(before));
        tisple::RunApple2(*a, 5000);
        if (frame < 30 || std::memcmp(before, a->cpu.mem + 0x0400, sizeof(before)) != 0) {
            continue; // the loader is still being read, or the meter is counting
        }
        int percent = LoadingScreen(*a);
        clean = clean && percent >= 0;
        if (percent >= 0 && (shown.empty() || shown.back() != percent)) {
            shown.push_back(percent);
        }
    }
    CHECK(results, clean);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    std::vector<int> want = {0, 29, 59, 61, 90, 98, 100};
    CHECK(results, shown == want);
    if (shown != want) {
        for (int p : shown) {
            std::fprintf(stderr, "meter: %d%%\n", p);
        }
    }
    for (const MemorySegment& s : segments) {
        CHECK(results, Loaded(*a, s));
    }
}

// the largest loader, ten segments, still fits its three sectors with the meter
void TestTenSegmentLoader(TestResults& results) {
    tisple::LoaderPlan plan;
    plan.entry = 0x6000;
    plan.slot_at = 0x1706;
    for (size_t i = 0; i < tisple::kMaxBootSegments; i++) {
        tisple::LoaderSegment segment;
        segment.first_page = static_cast<uint8_t>(0x10 + i * 0x10);
        segment.pages = 40;
        segment.bank = i == 0 ? tisple::LoaderBank::kAux : tisple::LoaderBank::kMain;
        plan.segments.push_back(segment);
    }
    uint8_t out[tisple::kBootLoaderSectors][tisple::kSectorBytes];
    uint16_t return_pc = 0;
    std::string error;
    bool built = tisple::BuildLoader(plan, out, return_pc, error);
    CHECK(results, built);
    if (!built) {
        std::fprintf(stderr, "%s\n", error.c_str());
    }
}

} // namespace

void RunDiskSegmentTests(TestResults& results) {
    TestSegmentsBoot(results);
    TestBank1Boot(results);
    TestCompiledRegionsBoot(results, 0);
    TestCompiledRegionsBoot(results, 1);
    TestCompiledRegionsBoot(results, 0, 1);
    TestSharedPagesBoot(results);
    TestBootMeter(results);
    TestTenSegmentLoader(results);
}

} // namespace tisple_tests
