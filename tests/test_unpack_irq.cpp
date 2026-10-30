// lib/unpack and lib/dhgr's code under lib/mockingboard's ticks with (auxmem.guard): a packed
// picture unpacked aux to aux and aux to main, and a sprite copied into aux memory, again and
// again while the card interrupts; ticks land while RAMRD and RAMWRT are on, none is lost, and
// every byte comes out right. Through the card's ram vector, as with a program in the card.

#include "machine/mockingboard.hpp"
#include "test_mockingboard.hpp"
#include "test_pack.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

constexpr uint16_t kGuardFix = 0x034C; // where the guard goes when RAMRD or RAMWRT is on
constexpr uint16_t kTicks = 0x03C0;

std::string Program(const std::vector<uint8_t>& picture) {
    std::string bytes;
    for (uint8_t b : picture) {
        bytes += (bytes.empty() ? "" : " ") + std::to_string(b);
    }
    return R"(
(import auxmem)
(import dhgr)
(import tables)
(import unpack)
(import mockingboard mb)
(import mockingboard_song tune)
(tune.song hum (pattern a "l1 v12 c4 e4 g4 c5") (voice 0 a))
(tables.packed_bytes picture ()" +
           bytes + R"() :in aux)
(let out_aux (array u8 8192) :in aux)
(let out_main (array u8 8192))
(let sprite (array u8 64) :in aux)
(let ticks u16 0 :address 0x03C0)
(fn irq () void :interrupt
    (mb.service)
    (set ticks (+ ticks 1)))
(fn main () void
    (mb.init)
    (auxmem.guard)
    (mb.play (addr_of (at hum 0)))
    (for r u8 0 3
        (unpack.aux_to_aux (addr_of (at out_aux 0)) (addr_of (at picture 0)))
        (unpack.aux_to_main (addr_of (at out_main 0)) (addr_of (at picture 0)))
        (auxmem.copy (addr_of (at sprite 0)) (addr_of (at out_aux 100)) 64)
        (for k u8 0 40
            (dhgr.copy dhgr.PAGE1 dhgr.AUX 4 (* k 4) 8 8 (addr_of (at sprite 0)))))
    (mb.off))
)";
}

struct Watch {
    const tisple::Apple2* a = nullptr;
    uint64_t in_read = 0;
    uint64_t in_write = 0;
};

void WatchGuard(void* ctx, const tisple::Machine& m) {
    Watch& w = *static_cast<Watch*>(ctx);
    if (tisple::GetRegs(m).pc != kGuardFix) {
        return;
    }
    w.in_read += w.a->sw.ramrd ? 1 : 0;
    w.in_write += w.a->sw.ramwrt ? 1 : 0;
}

int RowAddress(int y) {
    return 0x2000 + (y & 7) * 0x400 + ((y >> 3) & 7) * 0x80 + (y >> 6) * 0x28;
}

} // namespace

void RunUnpackIrqTests(TestResults& results) {
    std::vector<uint8_t> picture = DhgrSample(11, 1);
    MbBuild b;
    MbBuildText(results, "t.tsp", Program(picture), b); // packed by tables.packed_bytes
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    a->lc.write_enable = true; // as a program's startup in the card leaves it
    Watch watch;
    watch.a = a.get();
    tisple::RunApple2Traced(*a, 600 * kMbTick, WatchGuard, &watch);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    uint16_t out_aux = MbVarAddress(b, "t.out_aux");
    uint16_t out_main = MbVarAddress(b, "t.out_main");
    bool bytes = true;
    for (size_t i = 0; i < picture.size(); i++) {
        bytes =
            bytes && a->aux[out_aux + i] == picture[i] && a->cpu.mem[out_main + i] == picture[i];
    }
    CHECK(results, bytes);
    // the sprite: 8 bytes a row of out_aux from byte 100, drawn 8 rows tall every 4 rows, so
    // row y last got the sprite's row y % 4
    bool sprite = true;
    for (int y = 0; y < 160; y++) {
        for (int c = 0; c < 8; c++) {
            sprite = sprite && a->aux[RowAddress(y) + 4 + c] == picture[100 + (y % 4) * 8 + c];
        }
    }
    CHECK(results, sprite);
    uint64_t counted = a->cpu.mem[kTicks] | (a->cpu.mem[kTicks + 1] << 8);
    uint64_t due = a->cpu.cycles / kMbTick;
    CHECK(results, counted == a->cpu.irqs_taken && counted + 2 >= due && counted <= due);
    CHECK(results, watch.in_read > 3 && watch.in_write > 3);
    std::printf(
        "unpack under interrupts: %llu ticks, %llu landed with RAMRD on, %llu with RAMWRT\n",
        static_cast<unsigned long long>(counted), static_cast<unsigned long long>(watch.in_read),
        static_cast<unsigned long long>(watch.in_write));
}

} // namespace tisple_tests
