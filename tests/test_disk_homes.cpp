// the libraries' machine code at their fixed homes (lib/unpack at $1C00, lib/auxcard at $1DA6,
// lib/dhgr at $1E00): valued :address lets, which a boot disk's loader puts in place as segments
// of their own (BuildOptions::homes_in_segments) and other builds' startup copies in from an
// image. Either way the program unpacks, and each home holds its code in both memories after
// use; on a boot disk the startup has no copy and the disk no more segments for them

#include "backend/backend.hpp"
#include "disk/disk_command.hpp"
#include "test_apple2_libs.hpp"
#include "test_disk_read.hpp"

#include <cstdio>
#include <cstring>

namespace tisple_tests {

namespace {

constexpr uint16_t kOutMain = 0x9000;
constexpr uint16_t kOutAux = 0x9400;

std::vector<uint8_t> Picture() {
    std::vector<uint8_t> out(700);
    for (size_t i = 0; i < out.size(); i++) {
        out[i] = static_cast<uint8_t>((i / 7) % 5 == 0 ? 0x2A : i * 13);
    }
    return out;
}

std::string HomesProgram() {
    std::string bytes;
    for (uint8_t b : Picture()) {
        bytes += (bytes.empty() ? "" : " ") + std::to_string(b);
    }
    return "(import unpack)\n(import tables)\n(import dhgr)\n(import auxcard)\n"
           "(tables.packed_bytes PIC (" +
           bytes +
           "))\n"
           "(let out (array u8 1024) :address 0x9000 :reserve)\n"
           "(let out_aux (array u8 1024) :address 0x9400 :in aux :reserve)\n"
           "(let bank u8 0)\n"
           "(fn main () void\n"
           "    (unpack.main_to_main (as (ptr u8) 0x9000) (addr_of (at PIC 0)))\n"
           "    (unpack.main_to_aux (as (ptr u8) 0x9400) (addr_of (at PIC 0)))\n"
           "    (dhgr.setup)\n"
           "    (set bank auxcard.BANK2))\n";
}

// each library's home holds its let's value, in main memory and in aux memory after use
bool HomesIn(const tisple::Program& program, const tisple::Apple2& a) {
    const char* names[] = {"unpack.code", "auxcard.code", "dhgr.kit"};
    bool ok = true;
    for (const char* name : names) {
        int32_t v = tisple::FindVar(program, name);
        if (v < 0) {
            std::fprintf(stderr, "homes: no %s\n", name);
            return false;
        }
        const tisple::Var& var = program.vars[static_cast<size_t>(v)];
        uint16_t at = var.placement.address;
        bool main_ok = std::memcmp(a.cpu.mem + at, var.init.data(), var.init.size()) == 0;
        // auxcard's runs only from main memory (its code is never read with RAMRD on)
        bool aux_ok = std::strcmp(name, "auxcard.code") == 0 ||
                      std::memcmp(a.aux + at, var.init.data(), var.init.size()) == 0;
        if (!main_ok || !aux_ok) {
            std::fprintf(stderr, "homes: %s main %d aux %d\n", name, main_ok, aux_ok);
        }
        ok = ok && main_ok && aux_ok;
    }
    return ok;
}

bool Unpacked(const tisple::Apple2& a) {
    std::vector<uint8_t> want = Picture();
    return std::memcmp(a.cpu.mem + kOutMain, want.data(), want.size()) == 0 &&
           std::memcmp(a.aux + kOutAux, want.data(), want.size()) == 0;
}

void TestOnBootDisk(TestResults& results) {
    DiskProgram p;
    BuildDiskProgram(results, HomesProgram(), DiskReadLayout(0x8000), p);
    if (!p.ok) {
        return;
    }
    // the homes are segments: each of the three, loaded by the boot disk's loader
    int homes = 0;
    for (const tisple::MemorySegment& s : p.code.segments) {
        homes += (s.address == 0x1C00 || s.address == 0x1DA6 || s.address == 0x1E00) &&
                         s.bank == tisple::MemoryBank::kMain
                     ? 1
                     : 0;
    }
    CHECK(results, homes == 3);
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    RunDiskProgram(*a, kDiskReadBudget);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, Unpacked(*a));
    CHECK(results, HomesIn(p.program, *a));
    std::printf("library homes: on a boot disk the loader puts them in place (%zu segments, "
                "the program %zu bytes)\n",
                p.info.segments, p.code.image.size());
}

void TestStartupCopies(TestResults& results) {
    tisple::BuildOptions options;
    options.origin = 0x8000;
    options.limit = 0xBF00;
    LibBuild b;
    if (!BuildLibProgram(results, HomesProgram(), options, b)) {
        return;
    }
    // no segment for them: the startup copies them in from its images
    bool none = true;
    for (const tisple::MemorySegment& s : b.code.segments) {
        none = none && !(s.address >= 0x1C00 && s.address < 0x2000);
    }
    CHECK(results, none);
    std::unique_ptr<tisple::Apple2> a = LoadLibProgram(b);
    RunToReturn(results, *a, 20 * 1000 * 1000);
    CHECK(results, Unpacked(*a));
    CHECK(results, HomesIn(b.program, *a));
}

} // namespace

void RunDiskHomesTests(TestResults& results) {
    TestOnBootDisk(results);
    TestStartupCopies(results);
}

} // namespace tisple_tests
