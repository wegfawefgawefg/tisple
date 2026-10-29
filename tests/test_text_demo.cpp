// programs/text_demo, the bigger-program demo: more than 24 KB in main memory above and below
// the origin, the language card and aux memory. It runs loaded straight in, every chapter's
// status line names the memory the layout gave it and its first line is on the screen; and
// its disk boots to the same screen with our roms and with Apple's, and returns on ESC.

#include "disk/boot_loader.hpp"
#include "machine/disk2.hpp"
#include "test_backend.hpp"
#include "test_disk.hpp"

#include <cstdio>
#include <cstring>

namespace tisple_tests {

namespace {

using tisple::MemoryBank;

constexpr uint64_t kSettle = 4 * 1000 * 1000;       // a draw takes well under this
constexpr uint64_t kBootTime = 30ull * 1000 * 1000; // the 20-track boot takes about 11 M
constexpr int kChapters = 6;
const char* const kChapterVars[kChapters] = {
    "chapters.intro", "chapters.built",  "chapters.charset",
    "chapters.sines", "chapters.primes", "chapters.numbers",
};

// compiled as tisple build --disk does
bool CompileDemo(TestResults& results, Compiled& c) {
    c.built = BuildFile("programs/text_demo/main.tsp");
    CHECK(results, c.built.ok);
    if (!c.built.ok) {
        std::fprintf(stderr, "%s", ErrorText(c.built).c_str());
        return false;
    }
    tisple::BuildOptions options;
    options.limit = 0xC000;
    options.low_start = tisple::kBootLoadLow;
    std::vector<tisple::Diagnostic> errors;
    c.ok = tisple::CompileProgram(c.built.program, options, c.code, errors);
    CHECK(results, c.ok);
    if (!c.ok) {
        std::fprintf(stderr, "%s",
                     tisple::FormatDiagnostics(c.built.program.sources, errors).c_str());
    }
    return c.ok;
}

// row r of the 80-column screen as text, aux and main bytes in turn; inverse letters as
// plain ones (with the alternate set, inverse upper case is $00-$1F)
std::string Row80(const tisple::Apple2& a, int r) {
    int base = 0x0400 + (r % 8) * 0x80 + (r / 8) * 40;
    std::string text;
    for (int c = 0; c < 80; c++) {
        uint8_t code = (c % 2) == 0 ? a.aux[base + c / 2] : a.cpu.mem[base + c / 2];
        code &= 0x7F;
        text += static_cast<char>(code < 0x20 ? code + 0x40 : code);
    }
    return text;
}

// the 80 screen codes of row r against bytes from memory
bool RowIs(const tisple::Apple2& a, int r, const uint8_t* codes) {
    int base = 0x0400 + (r % 8) * 0x80 + (r / 8) * 40;
    for (int c = 0; c < 80; c++) {
        uint8_t code = (c % 2) == 0 ? a.aux[base + c / 2] : a.cpu.mem[base + c / 2];
        if (code != codes[c]) {
            return false;
        }
    }
    return true;
}

// what the status line says about the chapter at address in this memory
const char* PlaceName(bool aux, uint16_t address) {
    if (aux) {
        return "aux memory";
    }
    if (address >= 0xD000) {
        return "the language card";
    }
    return address < 0x6000 ? "main memory below $6000" : "main memory from $6000";
}

// a chapter's first line, from the segment that holds it; empty if none does
std::vector<uint8_t> ChapterLine(const Compiled& c, int chapter) {
    std::int32_t var = tisple::FindVar(c.built.program, kChapterVars[chapter]);
    if (var < 0) {
        return {};
    }
    bool in_aux = c.built.program.vars[static_cast<std::size_t>(var)].placement.in_aux;
    uint32_t at = c.code.layout.vars[static_cast<std::size_t>(var)].address;
    std::vector<tisple::MemorySegment> segments = tisple::ProgramSegments(c.code);
    for (const tisple::MemorySegment& s : segments) {
        bool aux = s.bank == MemoryBank::kAux;
        if (aux == in_aux && at >= s.address && at + 80 <= s.address + s.bytes.size()) {
            auto first = s.bytes.begin() + (at - s.address);
            return std::vector<uint8_t>(first, first + 80);
        }
    }
    return {};
}

void PressAndSettle(tisple::Apple2& a, uint8_t key) {
    tisple::ScheduleKey(a, tisple::Cycles(a) + 100, key);
    tisple::RunApple2(a, kSettle);
}

// RETURN through every chapter: the status line and the first line of each
void CheckChapters(TestResults& results, const Compiled& c, tisple::Apple2& a) {
    for (int chapter = 0; chapter < kChapters; chapter++) {
        if (chapter > 0) {
            PressAndSettle(a, 0x0D);
        }
        std::int32_t var = tisple::FindVar(c.built.program, kChapterVars[chapter]);
        CHECK(results, var >= 0);
        if (var < 0) {
            return;
        }
        bool aux = c.built.program.vars[static_cast<std::size_t>(var)].placement.in_aux;
        uint16_t at = c.code.layout.vars[static_cast<std::size_t>(var)].address;
        char want[80];
        std::snprintf(want, sizeof(want), " chapter %d is in %s ", chapter + 1, PlaceName(aux, at));
        std::string status = Row80(a, 23);
        std::vector<uint8_t> first = ChapterLine(c, chapter);
        bool ok = status.find(want) != std::string::npos && first.size() == 80 &&
                  RowIs(a, 1, first.data());
        if (!ok) {
            std::fprintf(stderr, "text demo chapter %d: status '%s', want '%s'\n", chapter + 1,
                         status.c_str(), want);
        }
        CHECK(results, ok);
    }
}

void TestDemoRuns(TestResults& results, const Compiled& c) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    std::string error;
    for (const tisple::MemorySegment& s : c.code.segments) {
        CHECK(results, tisple::LoadSegment(*a, s, error));
    }
    CHECK(results,
          tisple::LoadProgram(*a, c.code.image.data(), c.code.image.size(), c.code.origin, error));
    tisple::RunApple2(*a, kSettle);
    CHECK(results, a->stop_reason == tisple::MachineStop::kRunning);
    CHECK(results, Row80(*a, 0).find("Tisple ][ text viewer") != std::string::npos);
    CHECK(results, a->sw.col80 && a->lc.read_ram); // 80 columns; the card's ram is in
    CheckChapters(results, c, *a);
    PressAndSettle(*a, 0x1B); // ESC
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, !a->sw.col80 && !a->lc.read_ram);
}

// the disk through the given roms (none: ours), to the first page, then a return on ESC
void TestDemoBoots(TestResults& results, const Compiled& c, const std::vector<uint8_t>& main_rom,
                   const std::vector<uint8_t>& disk_rom) {
    auto disk = std::make_unique<tisple::DiskSectors>();
    tisple::BootDiskInfo info;
    std::string error;
    CHECK(results,
          tisple::MakeBootDisk(tisple::ProgramSegments(c.code), c.code.origin, *disk, info, error));
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*disk);
    a->return_pc = info.return_pc;
    if (main_rom.empty()) {
        tisple::StartDiskBoot(*a);
    } else {
        CHECK(results, tisple::LoadRom(*a, main_rom.data(), main_rom.size(), error));
        std::memcpy(a->slots.disk2.rom, disk_rom.data(), 256);
        tisple::ResetApple2(*a);
    }
    uint64_t start = tisple::Cycles(*a);
    while (tisple::Cycles(*a) - start < kBootTime &&
           Row80(*a, 0).find("Tisple ][ text viewer") == std::string::npos) {
        tisple::RunApple2(*a, tisple::kCyclesPerFrame);
    }
    uint64_t took = tisple::Cycles(*a) - start;
    tisple::RunApple2(*a, kSettle);
    bool drawn = Row80(*a, 0).find("Tisple ][ text viewer") != std::string::npos;
    if (drawn) {
        std::printf("text demo: boots to its first page in %.1f s (%s roms)\n",
                    static_cast<double>(took) / 1023000.0, main_rom.empty() ? "our" : "Apple's");
    }
    if (!drawn) {
        std::fprintf(stderr, "text demo did not boot (%s roms): %s\n",
                     main_rom.empty() ? "our" : "Apple's", tisple::DescribeStop(*a).c_str());
    }
    CHECK(results, drawn);
    if (!drawn) {
        return;
    }
    CheckChapters(results, c, *a);
    PressAndSettle(*a, 0x1B);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, a->stop_pc == info.return_pc);
}

} // namespace

void RunTextDemoTests(TestResults& results) {
    Compiled c;
    if (!CompileDemo(results, c)) {
        return;
    }
    // more than one stretch of memory could hold, in all four places
    size_t total = 0;
    bool low = false;
    bool card = false;
    bool aux = false;
    for (const tisple::MemorySegment& s : tisple::ProgramSegments(c.code)) {
        total += s.bytes.size();
        low = low || (s.bank == MemoryBank::kMain && s.address < c.code.origin);
        card = card || s.bank == MemoryBank::kCard;
        aux = aux || s.bank == MemoryBank::kAux;
    }
    CHECK(results, total > 24 * 1024 && low && card && aux && c.code.card_ram);
    CheckSegmentsApart(results, c);

    TestDemoRuns(results, c);
    TestDemoBoots(results, c, {}, {});
    std::vector<uint8_t> main_rom = OwnerRom("Apple2e.rom");
    std::vector<uint8_t> disk_rom = OwnerRom("DISK2.rom");
    if (main_rom.empty() || disk_rom.size() != 256) {
        std::printf("text demo: Apple ROMs not found, skipping the Apple ROM boot\n");
        return;
    }
    TestDemoBoots(results, c, main_rom, disk_rom);
}

} // namespace tisple_tests
