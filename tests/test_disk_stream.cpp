// examples/disk_stream booted from its disk: each picture read while the ball moves (step
// called after every sector but the last), then shown, the page shown holding the picture
// before it, but for the ball, each time a read begins

#include "disk/disk_command.hpp"
#include "pack/zx0.hpp"
#include "test_disk_read.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

constexpr uint16_t kBegin = 0x1700; // lib/disk's BEGIN

struct Stream {
    const tisple::Apple2* a = nullptr;
    uint16_t step = 0;
    std::vector<std::vector<uint8_t>> pictures; // in the order they are read
    int reads = 0;
    int steps = 0;
    std::vector<int> steps_per_read;
    int shown_right = 0; // reads that found the picture before on the page shown
    int most_off = 0;
};

void WatchStream(void* ctx, const tisple::Machine& m) {
    Stream& s = *static_cast<Stream*>(ctx);
    uint16_t pc = tisple::GetRegs(m).pc;
    if (pc == s.step) {
        s.steps++;
        return;
    }
    if (pc != kBegin) {
        return;
    }
    if (s.reads > 0) {
        s.steps_per_read.push_back(s.steps);
        const std::vector<uint8_t>& want = s.pictures[static_cast<size_t>((s.reads - 1) % 4)];
        uint16_t page = s.a->sw.page2 ? 0x4000 : 0x2000;
        int off = 0;
        for (size_t i = 0; i < want.size(); i++) {
            off += s.a->cpu.mem[page + i] != want[i] ? 1 : 0;
        }
        s.most_off = off > s.most_off ? off : s.most_off;
        s.shown_right += off <= 16 ? 1 : 0; // the ball is 2 bytes by 8 rows
    }
    s.steps = 0;
    s.reads++;
}

} // namespace

void RunDiskStreamTests(TestResults& results) {
    Built built = BuildFile("examples/disk_stream.tsp");
    CHECK(results, built.ok);
    if (!built.ok) {
        std::fprintf(stderr, "%s", ErrorText(built).c_str());
        return;
    }
    DiskProgram p;
    p.program = std::move(built.program);
    std::vector<tisple::Diagnostic> errors;
    p.ok = tisple::CompileProgram(p.program, DiskReadLayout(0x6000), p.code, errors);
    CHECK(results, p.ok);
    if (!p.ok) {
        return;
    }
    p.disk = std::make_unique<tisple::DiskSectors>();
    std::string error;
    p.ok = tisple::MakeBootDisk(tisple::ProgramSegments(p.code), p.code.origin, *p.disk, p.info,
                                error, tisple::BootExtrasFor(p.program));
    CHECK(results, p.ok && p.program.disk_tables.size() == 4);
    if (!p.ok || p.program.disk_tables.size() != 4) {
        return;
    }
    Stream s;
    for (size_t k = 0; k < 4; k++) {
        const std::vector<uint8_t>& bytes = p.program.disk_tables[k].bytes;
        std::vector<uint8_t> picture = bytes;
        if (k % 2 == 1) {
            CHECK(results, tisple::UnpackZx0(bytes, 8192, picture));
        }
        s.pictures.push_back(picture);
    }
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    s.a = a.get();
    s.step = DiskProgramPointer(p, "disk_stream.step");
    CHECK(results, s.step != 0);
    // 9 reads: two rounds of the four pictures and the next one begun
    for (int frame = 0; frame < 1200 && s.reads < 10; frame++) {
        tisple::RunApple2Traced(*a, tisple::kCyclesPerFrame, WatchStream, &s);
    }
    CHECK(results, s.reads == 10 && s.shown_right == 9);
    bool steps = s.steps_per_read.size() == 9;
    for (size_t n = 0; n < s.steps_per_read.size(); n++) {
        size_t sectors = p.program.disk_tables[n % 4].sectors.size();
        steps = steps && s.steps_per_read[n] == static_cast<int>(sectors) - 1;
    }
    CHECK(results, steps);
    std::printf("disk stream example: %d pictures shown, %.1f s from power on, the page shown "
                "off by at most %d bytes (the ball)\n",
                s.shown_right, static_cast<double>(tisple::Cycles(*a)) / 1023000.0, s.most_off);
}

} // namespace tisple_tests
