// the code size budget (backend/size_budget.hpp): copies of calls and unrolled loops that add
// many bytes for few cycles are turned down, the ones that pay for themselves stay

#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

// a small function with a loop, called at eight places with constant counts: copied at each
// call with its loop unrolled, this was 5.6 KB at -O0 (the Catwiz game's sounds)
const char* const kNoise =
    "(let seed u16 1)\n"
    "(fn rnd () u8 (set seed (^ seed (<< seed 7))) (set seed (^ seed (>> seed 9)))\n"
    "    (set seed (^ seed (<< seed 8))) (as u8 seed))\n"
    "(fn noise ((base u16) (count u8)) void\n"
    "    (for i u8 0 count (io_read u8 0xC030) (wait_cycles (+ base (<< (as u16 (rnd)) 2)))))\n"
    "(fn main () u8\n"
    "    (noise 600 18) (noise 1400 8) (noise 300 3) (noise 900 10)\n"
    "    (noise 70 100) (noise 1600 10) (noise 120 2) (noise 150 2) (as u8 seed))\n";

// a CRC step over 1 KB: its 8 bits unrolled save 34 cycles a byte for 184 bytes, and it runs
// 1024 times: that pays
const char* const kCrc =
    "(let data (array u8 1024))\n"
    "(fn main () u16 (let crc u16 0xFFFF)\n"
    "    (for i u16 0 1024 (set (at data i) (as u8 (* i 7))))\n"
    "    (for i u16 0 1024 (set crc (^ crc (<< (as u16 (at data i)) 8)))\n"
    "        (for k u8 0 8 (if (!= (& crc 0x8000) 0) (set crc (^ (<< crc 1) 0x1021))\n"
    "            (set crc (<< crc 1)))))\n"
    "    crc)\n";

void CheckRuns(TestResults& results, const Compiled& c, const char* what) {
    Diff d;
    d.in.max_steps = 2000000;
    d.max_cycles = 40000000;
    bool ok = StartDiff(d, c) && DiffCall(d, "t.main", {});
    if (!ok) {
        std::fprintf(stderr, "%s: %s\n", what, d.message.c_str());
    }
    CHECK(results, ok);
}

void TestNoiseStaysSmall(TestResults& results) {
    for (int level : {0, 1}) {
        tisple::OptOptions opt;
        opt.level = level;
        Compiled c = CompileMainText(kNoise, opt);
        CHECK(results, c.ok);
        if (!c.ok) {
            continue;
        }
        if (c.code.image.size() > 600) {
            std::fprintf(stderr, "noise at -O%d: %zu bytes\n", level, c.code.image.size());
        }
        CHECK(results, c.code.image.size() <= 600);
        CheckRuns(results, c, "noise");
    }
}

void TestHotUnrollStays(TestResults& results) {
    Compiled c = CompileMainText(kCrc);
    CHECK(results, c.ok);
    if (!c.ok) {
        return;
    }
    // one ROL a bit, each bit a copy (the fill loop shifts too)
    CHECK(results, CountMn(c, "t.main", tisple::Mn::ROL) >= 8);
    CheckRuns(results, c, "crc");
}

} // namespace

void RunBackendSizeTests(TestResults& results) {
    TestNoiseStaysSmall(results);
    TestHotUnrollStays(results);
}

} // namespace tisple_tests
