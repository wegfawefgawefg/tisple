// lib/disk on the built-in IIe: tables :on disk of every size around a page and a track, laid
// out 1:1, 2:1 and 3:1, read back byte for byte into main and aux memory with nothing past
// their ends written, a packed one unpacked after its read; and where the disk build puts
// them

#include "test_disk_read.hpp"

#include "disk/disk_command.hpp"
#include "disk/disk_reader.hpp"
#include "disk/disk_tables.hpp"
#include "disk/dos33.hpp"
#include "machine/disk2.hpp"
#include "test_disk.hpp"

#include <cstdio>

namespace tisple_tests {

tisple::BuildOptions DiskReadLayout(uint16_t origin) {
    tisple::BuildOptions options;
    tisple::UseBootDiskLayout(options);
    options.origin = origin;
    return options;
}

void BuildDiskProgram(TestResults& results, const std::string& text,
                      const tisple::BuildOptions& options, DiskProgram& out) {
    tisple::LoadOptions load;
    load.files.push_back(tisple::VirtualFile{"t.tsp", text});
    std::vector<tisple::Diagnostic> errors;
    out.ok = tisple::BuildProgram("t.tsp", load, out.program, errors) &&
             tisple::CompileProgram(out.program, options, out.code, errors);
    if (!out.ok) {
        std::fprintf(stderr, "%s", tisple::FormatDiagnostics(out.program.sources, errors).c_str());
        CHECK(results, out.ok);
        return;
    }
    out.disk = std::make_unique<tisple::DiskSectors>();
    std::string error;
    out.ok = tisple::MakeBootDisk(tisple::ProgramSegments(out.code), out.code.origin, *out.disk,
                                  out.info, error, tisple::BootExtrasFor(out.program));
    if (!out.ok) {
        std::fprintf(stderr, "disk: %s\n", error.c_str());
    }
    CHECK(results, out.ok);
}

std::unique_ptr<tisple::Apple2> BootDiskProgram(const DiskProgram& p) {
    std::unique_ptr<tisple::Apple2> a = MachineWithDisk(*p.disk);
    a->return_pc = p.info.return_pc;
    tisple::StartDiskBoot(*a);
    return a;
}

uint16_t DiskProgramVar(const DiskProgram& p, const char* name) {
    int32_t v = tisple::FindVar(p.program, name);
    return v < 0 ? 0 : p.code.layout.vars[static_cast<size_t>(v)].address;
}

uint16_t DiskProgramFunction(const DiskProgram& p, const char* name) {
    int32_t f = tisple::FindFunction(p.program, name);
    return f < 0 ? 0 : p.code.function_address[static_cast<size_t>(f)];
}

uint16_t DiskProgramPointer(const DiskProgram& p, const char* name) {
    int32_t f = tisple::FindFunction(p.program, name);
    if (f < 0 || static_cast<size_t>(f) >= p.code.layout.functions.size()) {
        return 0;
    }
    return p.code.layout.functions[static_cast<size_t>(f)];
}

const tisple::DiskTable* FindDiskTable(const DiskProgram& p, const char* name) {
    for (const tisple::DiskTable& table : p.program.disk_tables) {
        if (p.program.vars[static_cast<size_t>(table.var)].name == name) {
            return &table;
        }
    }
    return nullptr;
}

std::string MakeBytes(const std::vector<uint8_t>& bytes) {
    std::string text = "(make (array u8 " + std::to_string(bytes.size()) + ")";
    for (uint8_t b : bytes) {
        text += " " + std::to_string(b);
    }
    return text + ")";
}

std::vector<uint8_t> DiskPattern(size_t size, uint32_t salt) {
    std::vector<uint8_t> out(size);
    uint32_t x = 0x9E3779B9u ^ salt;
    for (size_t i = 0; i < size; i++) {
        x = x * 1103515245u + 12345u;
        out[i] = static_cast<uint8_t>((x >> 16) ^ (i >> 8));
    }
    return out;
}

uint64_t RunDiskProgram(tisple::Apple2& a, uint64_t budget) {
    uint64_t start = tisple::Cycles(a);
    while (!a.stopped && tisple::Cycles(a) - start < budget) {
        tisple::RunApple2(a, tisple::kCyclesPerFrame);
    }
    return tisple::Cycles(a) - start;
}

namespace {

struct Size {
    uint32_t bytes;
    int interleave; // 0: the default
};

const Size kSizes[] = {{1, 0},    {255, 0},  {256, 0},  {257, 0}, {1000, 1},
                       {4096, 0}, {4097, 3}, {6000, 0}, {9000, 2}};
constexpr int kTables = static_cast<int>(sizeof(kSizes) / sizeof(kSizes[0]));
constexpr uint16_t kMainAt = 0x2000;
constexpr uint16_t kAuxAt = 0x2000;
constexpr uint16_t kUnpackedMain = 0x8400;
constexpr uint16_t kStatus = 0x03C0;
constexpr uint8_t kUntouched = 0xA5;

// each table to main memory and to aux memory, a byte apart, then the packed one to aux and
// unpacked from there to main; each read's status in kStatus + n
std::string ExactProgram(const std::vector<std::vector<uint8_t>>& tables,
                         const std::vector<uint8_t>& picture) {
    std::string text = "(import disk)\n(import tables)\n(import unpack)\n";
    text += "(let status (array u8 32) :address 0x03C0)\n";
    text += "(let main_room (array u8 25600) :address 0x2000 :reserve)\n";
    text += "(let aux_room (array u8 25600) :address 0x2000 :in aux :reserve)\n";
    text += "(let packed_room (array u8 8192) :address 0x9000 :in aux :reserve)\n";
    text += "(let unpacked_room (array u8 8192) :address 0x8400 :reserve)\n";
    for (int k = 0; k < kTables; k++) {
        text += "(const T" + std::to_string(k) + " (array u8 " + std::to_string(kSizes[k].bytes) +
                ") " + MakeBytes(tables[static_cast<size_t>(k)]) + " :on disk";
        if (kSizes[k].interleave != 0) {
            text += " :interleave " + std::to_string(kSizes[k].interleave);
        }
        text += ")\n";
    }
    std::string bytes;
    for (uint8_t b : picture) {
        bytes += (bytes.empty() ? "" : " ") + std::to_string(b);
    }
    text += "(tables.packed_bytes PICTURE (" + bytes + ") :on disk)\n";
    text += "(fn main () void\n";
    uint32_t at = 0;
    for (int k = 0; k < kTables; k++) {
        std::string table = "(addr_of (at T" + std::to_string(k) + " 0))";
        std::string where = std::to_string(kMainAt + at);
        text += "    (set (at status " + std::to_string(2 * k) + ") (disk.read (as (ptr u8) " +
                where + ") " + table + " disk.MAIN))\n";
        text += "    (set (at status " + std::to_string(2 * k + 1) + ") (disk.read (as (ptr u8) " +
                where + ") " + table + " disk.AUX))\n";
        at += kSizes[k].bytes + 1;
    }
    text += "    (set (at status 30) (disk.read (as (ptr u8) 0x9000) (addr_of (at PICTURE 0)) "
            "disk.AUX))\n";
    text += "    (unpack.aux_to_main (as (ptr u8) 0x8400) (as (ptr u8) 0x9000)))\n";
    return text;
}

void TestExact(TestResults& results) {
    std::vector<std::vector<uint8_t>> tables;
    for (int k = 0; k < kTables; k++) {
        tables.push_back(DiskPattern(kSizes[k].bytes, static_cast<uint32_t>(k)));
    }
    std::vector<uint8_t> picture(6000);
    for (size_t i = 0; i < picture.size(); i++) {
        picture[i] = static_cast<uint8_t>((i / 40) % 7 == 0 ? 0x55 : (i * 3) / 160);
    }
    DiskProgram p;
    BuildDiskProgram(results, ExactProgram(tables, picture), DiskReadLayout(0xA800), p);
    if (!p.ok) {
        return;
    }
    // the tables' places: the last tracks, the program's below them
    CHECK(results, p.info.table_track > p.info.last_track && p.info.table_track >= 27);
    const tisple::DiskTable* t4 = FindDiskTable(p, "t.T4");
    const tisple::DiskTable* t8 = FindDiskTable(p, "t.T8");
    CHECK(results, t4 != nullptr && t4->sectors.size() == 4);
    CHECK(results, t8 != nullptr && t8->sectors.size() == 36 && t8->tracks.back() == 34);
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    for (uint32_t i = 0; i < 0xA000 - 0x2000; i++) {
        a->cpu.mem[0x2000 + i] = kUntouched;
        a->aux[0x2000 + i] = kUntouched;
    }
    uint64_t took = RunDiskProgram(*a, kDiskReadBudget);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    bool statuses = true;
    for (int n = 0; n < 2 * kTables; n++) {
        statuses = statuses && a->cpu.mem[kStatus + n] == tisple::kReaderDone;
    }
    CHECK(results, statuses && a->cpu.mem[kStatus + 30] == tisple::kReaderDone);
    uint32_t at = 0;
    for (int k = 0; k < kTables; k++) {
        const std::vector<uint8_t>& want = tables[static_cast<size_t>(k)];
        bool main_ok = true;
        bool aux_ok = true;
        for (size_t i = 0; i < want.size(); i++) {
            main_ok = main_ok && a->cpu.mem[kMainAt + at + i] == want[i];
            aux_ok = aux_ok && a->aux[kAuxAt + at + i] == want[i];
        }
        at += kSizes[k].bytes;
        // the byte after each table: never written
        main_ok = main_ok && a->cpu.mem[kMainAt + at] == kUntouched;
        aux_ok = aux_ok && a->aux[kAuxAt + at] == kUntouched;
        at++;
        if (!main_ok || !aux_ok) {
            std::fprintf(stderr, "disk read: table %d (%u bytes) main %d aux %d\n", k,
                         kSizes[k].bytes, main_ok, aux_ok);
        }
        CHECK(results, main_ok && aux_ok);
    }
    bool unpacked = true;
    for (size_t i = 0; i < picture.size(); i++) {
        unpacked = unpacked && a->cpu.mem[kUnpackedMain + i] == picture[i];
    }
    CHECK(results, unpacked);
    CHECK(results, !a->sw.ramwrt && !a->sw.ramrd);
    CHECK(results, !tisple::DriveSpinning(*a, tisple::Cycles(*a) + tisple::kMotorOffDelay));
    std::printf("disk read: %d tables (%u KB) to main and aux and a packed one, %.1f s from "
                "power on\n",
                kTables, at * 2 / 1024, static_cast<double>(took) / 1023000.0);
}

// the tables' places: declaration order from the end, interleaves, past track 17, too many
void TestPlaces(TestResults& results) {
    std::vector<tisple::DiskTable> tables(3);
    tables[0].bytes.assign(512, 1);
    tables[1].bytes.assign(16 * 256 + 1, 2);
    tables[1].interleave = 3;
    tables[2].bytes.assign(3 * 256, 3);
    tables[2].interleave = 1;
    std::string error;
    CHECK(results, tisple::PlaceDiskTables(tables, error));
    // 2 + 17 + 3 sectors: two tracks, the first table at the start of track 33
    CHECK(results, tables[0].tracks[0] == 33 && tables[0].sectors[0] == 0);
    CHECK(results, tables[0].sectors[1] == 2);
    // the next one goes on from there, 3 apart, onto track 34 for its last sectors
    CHECK(results, tables[1].tracks[0] == 33 && tables[1].sectors[0] == 5);
    CHECK(results, tables[1].tracks.back() == 34);
    CHECK(results, tables[2].tracks[0] == 34);
    std::vector<uint8_t> record = tisple::DiskTableRecord(tables[1]);
    CHECK(results, record.size() == 3 + 17 && record[0] == 1 && record[1] == 16 && record[2] == 33);
    CHECK(results, (record.back() >> 4) == 1);
    // every sector used once
    bool seen[35][16] = {};
    bool once = true;
    for (const tisple::DiskTable& t : tables) {
        for (size_t k = 0; k < t.tracks.size(); k++) {
            once = once && !seen[t.tracks[k]][t.sectors[k]];
            seen[t.tracks[k]][t.sectors[k]] = true;
        }
    }
    CHECK(results, once);
    // across the catalog's track
    std::vector<tisple::DiskTable> big(1);
    big[0].bytes.assign(18 * 4096, 0);
    CHECK(results, !tisple::PlaceDiskTables(big, error)); // more than the 48 KB a table holds
    big.assign(2, tisple::DiskTable());
    big[0].bytes.assign(9 * 4096, 0);
    big[1].bytes.assign(9 * 4096, 0);
    CHECK(results, tisple::PlaceDiskTables(big, error));
    bool skips = true;
    for (const tisple::DiskTable& t : big) {
        for (uint8_t track : t.tracks) {
            skips = skips && track != tisple::kBootCatalogTrack;
        }
    }
    CHECK(results, skips && big[0].tracks[0] == 16);
    std::vector<tisple::DiskTable> full(4);
    for (tisple::DiskTable& t : full) {
        t.bytes.assign(9 * 4096, 0);
    }
    CHECK(results, !tisple::PlaceDiskTables(full, error));
    CHECK(results, error.find("528") != std::string::npos);
}

// what the checker says
void TestCheckErrors(TestResults& results) {
    struct Case {
        const char* text;
        const char* message;
    };
    const Case cases[] = {
        {"(const T (array u8 2) (make (array u8 2) 1 2) :on tape)\n(fn main () void)",
         ":on takes disk"},
        {"(const T (array u16 2) (make (array u16 2) 1 2) :on disk)\n(fn main () void)",
         "a table :on disk holds bytes"},
        {"(const T (array u8 2) (make (array u8 2) 1 2) :interleave 2)\n(fn main () void)",
         ":interleave goes with :on disk"},
        {"(const T (array u8 2) (make (array u8 2) 1 2) :on disk :interleave 16)\n"
         "(fn main () void)",
         "1 to 15"},
        {"(let t (array u8 2) (make (array u8 2) 1 2) :on disk)\n(fn main () void)",
         "is for const tables"},
        {"(const T (array u8 2) (make (array u8 2) 1 2) :on disk :align 256)\n"
         "(fn main () void)",
         "has no address in memory"},
        {"(const T (array u8 2) (make (array u8 2) 1 2) :on disk)\n"
         "(let x u8 0)\n(fn main () void (set x (at T 1)))",
         "T is on the disk, not in memory"},
    };
    for (const Case& c : cases) {
        tisple::LoadOptions load;
        load.files.push_back(tisple::VirtualFile{"t.tsp", c.text});
        tisple::Program program;
        std::vector<tisple::Diagnostic> errors;
        bool ok = tisple::BuildProgram("t.tsp", load, program, errors);
        std::string shown = tisple::FormatDiagnostics(program.sources, errors);
        bool found = !ok && shown.find(c.message) != std::string::npos;
        if (!found) {
            std::fprintf(stderr, "disk table check: want \"%s\", got:\n%s", c.message,
                         shown.c_str());
        }
        CHECK(results, found);
    }
}

} // namespace

void RunDiskReadTests(TestResults& results) {
    TestPlaces(results);
    TestCheckErrors(results);
    TestExact(results);
    RunDiskReadFailTests(results);
    RunDiskReadTimingTests(results);
    RunDiskStreamTests(results);
}

} // namespace tisple_tests
