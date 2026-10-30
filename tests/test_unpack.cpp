// lib/unpack's 6502 code on the built-in IIe: packed bytes put into main or aux memory, the
// program unpacks them with each of the four modes, and every byte out (and the one after) is
// checked against the plain decoder; random and made-up picture data. Prints the cycles a byte
// on the pictures.

#include "pack/zx0.hpp"
#include "test_mockingboard.hpp"
#include "test_pack.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

constexpr uint16_t kSrc = 0x9600; // packed bytes, up to 10.5 KB (past the zero-page save)
constexpr uint16_t kDst = 0x2000; // out, up to 16 KB
constexpr uint16_t kMode = 0x03C0;
constexpr uint16_t kHome = 0x1C00; // where lib/unpack's code runs

const char* kProgram = R"(
(import unpack)
(let src_main (array u8 10752) :address 0x9600 :reserve)
(let src_aux (array u8 10752) :address 0x9600 :in aux :reserve)
(let dst_main (array u8 16384) :address 0x2000 :reserve)
(let dst_aux (array u8 16384) :address 0x2000 :in aux :reserve)
(let mode u8 :address 0x03C0)
(fn main () void
    (let src (ptr u8) (as (ptr u8) 0x9600))
    (let dst (ptr u8) (as (ptr u8) 0x2000))
    (match mode
        (0 (unpack.main_to_main dst src))
        (1 (unpack.aux_to_main dst src))
        (2 (unpack.main_to_aux dst src))
        (_ (unpack.aux_to_aux dst src))))
)";

// cycles spent in lib/unpack's code, from its first instruction to its return
struct InHome {
    uint64_t last = 0;
    bool inside = false;
    uint64_t cycles = 0;
};

void CountHome(void* ctx, const tisple::Machine& m) {
    InHome& h = *static_cast<InHome*>(ctx);
    uint16_t pc = tisple::GetRegs(m).pc;
    bool now = pc >= kHome && pc < kHome + 0x200;
    if (h.inside) {
        h.cycles += m.cycles - h.last;
    }
    h.inside = now;
    h.last = m.cycles;
}

struct Case {
    std::vector<uint8_t> data;
    int mode = 0;
};

// false, with what went wrong, unless every byte lands where it belongs
bool RunCase(const MbBuild& b, const Case& c, uint64_t& cycles) {
    std::vector<uint8_t> packed = tisple::PackZx0(c.data);
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 0);
    bool src_aux = (c.mode & 1) != 0;
    bool dst_aux = (c.mode & 2) != 0;
    uint8_t* src = src_aux ? a->aux : a->cpu.mem;
    uint8_t* dst = dst_aux ? a->aux : a->cpu.mem;
    for (size_t i = 0; i < packed.size(); i++) {
        src[kSrc + i] = packed[i];
    }
    for (size_t i = 0; i <= c.data.size(); i++) {
        dst[kDst + i] = 0xA5; // so a byte not written shows
    }
    a->cpu.mem[kMode] = static_cast<uint8_t>(c.mode);
    InHome home;
    tisple::RunApple2Traced(*a, 40 * 1000 * 1000, CountHome, &home);
    cycles = home.cycles;
    if (a->stop_reason != tisple::MachineStop::kReturned) {
        std::fprintf(stderr, "unpack: mode %d, %zu bytes: %s\n", c.mode, c.data.size(),
                     tisple::DescribeStop(*a).c_str());
        return false;
    }
    if (a->sw.ramrd || a->sw.ramwrt) {
        std::fprintf(stderr, "unpack: mode %d left RAMRD or RAMWRT on\n", c.mode);
        return false;
    }
    for (size_t i = 0; i < c.data.size(); i++) {
        if (dst[kDst + i] != c.data[i]) {
            std::fprintf(stderr, "unpack: mode %d, %zu bytes: byte %zu is $%02X, not $%02X\n",
                         c.mode, c.data.size(), i, dst[kDst + i], c.data[i]);
            return false;
        }
    }
    if (dst[kDst + c.data.size()] != 0xA5) {
        std::fprintf(stderr, "unpack: mode %d wrote past the end\n", c.mode);
        return false;
    }
    return true;
}

} // namespace

void RunUnpackLibTests(TestResults& results) {
    MbBuild b;
    MbBuildText(results, "t.tsp", kProgram, b);
    if (!b.ok) {
        return;
    }
    // the pictures in every mode, counted
    uint64_t picture_cycles = 0;
    size_t picture_bytes = 0;
    for (uint32_t seed = 0; seed < 4; seed++) {
        for (int mode = 0; mode < 4; mode++) {
            Case c;
            c.data = DhgrSample(seed, mode & 1);
            c.mode = mode;
            uint64_t cycles = 0;
            CHECK(results, RunCase(b, c, cycles));
            picture_cycles += cycles;
            picture_bytes += c.data.size();
        }
    }
    std::printf("unpack: made-up double hi-res pictures, %.1f cycles a byte out\n",
                static_cast<double>(picture_cycles) / static_cast<double>(picture_bytes));
    // random shapes and sizes, the mode by turns
    TestRandom r{77};
    int failed = 0;
    for (int k = 0; k < 48; k++) {
        Case c;
        int size = 1 + r.Below(k < 32 ? 700 : 9000);
        c.data = PackSample(r, size);
        c.mode = k & 3;
        uint64_t cycles = 0;
        failed += RunCase(b, c, cycles) ? 0 : 1;
    }
    CHECK(results, failed == 0);
    // a page and more of one byte: the page loops, and lengths past 255
    for (int mode = 0; mode < 4; mode++) {
        Case c;
        c.data.assign(16000, 0x2A);
        c.data[5000] = 1;
        c.mode = mode;
        uint64_t cycles = 0;
        CHECK(results, RunCase(b, c, cycles));
    }
}

} // namespace tisple_tests
