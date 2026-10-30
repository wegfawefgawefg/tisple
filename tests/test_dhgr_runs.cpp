// lib/dhgr's runs (sprite halves stored as runs) and the aux language card: made-up sprite
// halves encoded as runs, drawn from their own memory and from the aux language card (put there
// by unpack.aux_to_card and main_to_card), plain and white, under the Mockingboard's ticks
// with (auxmem.guard); every byte of the boxes against (screen & mask) | picture, none outside
// touched, the card's bytes as packed, no tick lost.

#include "machine/mockingboard.hpp"
#include "test_backend.hpp"
#include "test_mockingboard.hpp"
#include "test_pack.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace tisple_tests {

namespace {

constexpr int kW = 9; // bytes a row in each memory
constexpr int kH = 21;
constexpr uint16_t kTicks = 0x03C0;

struct Half {
    std::vector<uint8_t> pic;  // kW * kH
    std::vector<uint8_t> mask; // bit set: the screen shows through
    std::vector<uint8_t> runs;
};

// a row's bytes: clear, opaque or edged, in stretches, a run longer than 63 now and then
Half MakeHalf(TestRandom& r) {
    Half h;
    for (int y = 0; y < kH; y++) {
        int kind = r.Below(3);
        for (int x = 0; x < kW; x++) {
            if (r.Below(3) == 0) {
                kind = r.Below(3);
            }
            uint8_t m = kind == 0   ? 0x7F
                        : kind == 1 ? 0x00
                                    : static_cast<uint8_t>(r.Below(126) + 1);
            uint8_t p = static_cast<uint8_t>(r.Below(128)) & static_cast<uint8_t>(~m) & 0x7F;
            h.pic.push_back(kind == 0 ? 0 : p);
            h.mask.push_back(m);
        }
    }
    // runs: per row skips, opaque runs and masked runs (picture bytes, then masks), then 0
    for (int y = 0; y < kH; y++) {
        int x = 0;
        int end = kW;
        while (end > 0 && h.mask[y * kW + end - 1] == 0x7F) {
            end--;
        }
        while (x < end) {
            uint8_t m = h.mask[y * kW + x];
            int n = 1;
            auto same = [&](int i) {
                uint8_t mi = h.mask[y * kW + i];
                return m == 0x7F ? mi == 0x7F : m == 0 ? mi == 0 : mi != 0 && mi != 0x7F;
            };
            while (x + n < end && n < 63 && same(x + n)) {
                n++;
            }
            if (m == 0x7F) {
                h.runs.push_back(static_cast<uint8_t>(n));
            } else if (m == 0) {
                h.runs.push_back(static_cast<uint8_t>(0x40 + n));
                for (int i = 0; i < n; i++) {
                    h.runs.push_back(h.pic[y * kW + x + i]);
                }
            } else {
                h.runs.push_back(static_cast<uint8_t>(0x80 + n));
                for (int i = 0; i < n; i++) {
                    h.runs.push_back(h.pic[y * kW + x + i]);
                }
                for (int i = 0; i < n; i++) {
                    h.runs.push_back(h.mask[y * kW + x + i]);
                }
            }
            x += n;
        }
        h.runs.push_back(0);
    }
    return h;
}

std::string Bytes(const std::vector<uint8_t>& v) {
    std::string s;
    for (uint8_t b : v) {
        s += (s.empty() ? "" : " ") + std::to_string(b);
    }
    return s;
}

std::string Program(const Half& aux, const Half& main) {
    std::string na = std::to_string(aux.runs.size());
    std::string nm = std::to_string(main.runs.size());
    return R"(
(import auxmem)
(import dhgr)
(import tables)
(import unpack)
(import mockingboard mb)
(import mockingboard_song tune)
(tune.song hum (pattern a "l1 v12 c4 e4 g4 c5") (voice 0 a))
(let aux_runs (array u8 )" +
           na + R"() (make (array u8 )" + na + ") " + Bytes(aux.runs) +
           R"() :in aux)
(const MAIN_RUNS (array u8 )" +
           nm + R"() (make (array u8 )" + nm + ") " + Bytes(main.runs) +
           R"())
(tables.packed_bytes packed_aux ()" +
           Bytes(aux.runs) + R"() :in aux)
(tables.packed_bytes PACKED_MAIN ()" +
           Bytes(main.runs) + R"())
(let ticks u16 0 :address 0x03C0)
(fn irq () void :interrupt
    (mb.service)
    (set ticks (+ ticks 1)))
(let bank_after u8 0 :address 0x03C4)
(let rom_after u8 0 :address 0x03C5)
(fn main () void
    (mb.init)
    (auxmem.guard)
    (mb.play (addr_of (at hum 0)))
    (unpack.aux_to_card (as (ptr u8) 0xD000) (addr_of (at packed_aux 0)))
    (unpack.main_to_card (as (ptr u8) 0xE100) (addr_of (at PACKED_MAIN 0)))
    (dhgr.clear dhgr.PAGE1 dhgr.MEDIUM_BLUE)
    (dhgr.clear dhgr.PAGE2 dhgr.ORANGE)
    (for k u8 0 6
        (dhgr.runs dhgr.PAGE1 dhgr.AUX 4 10 )" +
           std::to_string(kH) + R"( (addr_of (at aux_runs 0)) 0)
        (dhgr.runs dhgr.PAGE1 dhgr.MAIN 4 10 )" +
           std::to_string(kH) + R"( (addr_of (at MAIN_RUNS 0)) 0)
        (dhgr.runs dhgr.PAGE2 dhgr.AUX 20 60 )" +
           std::to_string(kH) + R"( (as (ptr u8) 0xD000) dhgr.FROM_CARD)
        (dhgr.runs dhgr.PAGE2 dhgr.MAIN 20 60 )" +
           std::to_string(kH) + R"( (as (ptr u8) 0xE100) dhgr.FROM_CARD)
        (dhgr.runs dhgr.PAGE1 dhgr.AUX 26 150 )" +
           std::to_string(kH) + R"( (addr_of (at aux_runs 0)) dhgr.AS_WHITE)
        (dhgr.runs dhgr.PAGE1 dhgr.MAIN 26 150 )" +
           std::to_string(kH) + R"( (as (ptr u8) 0xE100) (| dhgr.FROM_CARD dhgr.AS_WHITE)))
    ; from bank 1 of the card and from the rom: each is in again after
    (io_read u8 0xC08B)
    (io_read u8 0xC08B)
    (dhgr.runs dhgr.PAGE2 dhgr.MAIN 20 60 )" +
           std::to_string(kH) + R"( (as (ptr u8) 0xE100) dhgr.FROM_CARD)
    (set bank_after (io_read u8 0xC011))
    (io_read u8 0xC082)
    (dhgr.runs dhgr.PAGE2 dhgr.MAIN 20 60 )" +
           std::to_string(kH) + R"( (as (ptr u8) 0xE100) dhgr.FROM_CARD)
    (set rom_after (io_read u8 0xC012))
    (io_read u8 0xC083)
    (io_read u8 0xC083)
    (mb.off))
)";
}

int RowAddress(int page, int y) {
    return page + (y & 7) * 0x400 + ((y >> 3) & 7) * 0x80 + (y >> 6) * 0x28;
}

// every byte of page's rows: inside the box at col, top the sprite over the background byte
// the page had there (bg_aux, bg_main by column parity), elsewhere the background
bool PageRight(const tisple::Apple2& a, int page, int col, int top, const Half& aux,
               const Half& main, bool white, const uint8_t bg_aux[2], const uint8_t bg_main[2]) {
    for (int y = top - 2; y < top + kH + 2; y++) {
        for (int c = 0; c < 40; c++) {
            int at = RowAddress(page, y) + c;
            uint8_t want_aux = bg_aux[c & 1];
            uint8_t want_main = bg_main[c & 1];
            if (y >= top && y < top + kH && c >= col && c < col + kW) {
                int i = (y - top) * kW + (c - col);
                want_aux = (want_aux & aux.mask[i]) | (white ? aux.mask[i] ^ 0x7F : aux.pic[i]);
                want_main =
                    (want_main & main.mask[i]) | (white ? main.mask[i] ^ 0x7F : main.pic[i]);
                if (white && aux.mask[i] == 0x7F) {
                    want_aux = bg_aux[c & 1];
                }
                if (white && main.mask[i] == 0x7F) {
                    want_main = bg_main[c & 1];
                }
            }
            if (a.aux[at] != want_aux || a.cpu.mem[at] != want_main) {
                std::printf("runs: page $%04X row %d column %d: aux %02X want %02X, main %02X "
                            "want %02X\n",
                            page, y, c, a.aux[at], want_aux, a.cpu.mem[at], want_main);
                return false;
            }
        }
    }
    return true;
}

} // namespace

void RunDhgrRunsTests(TestResults& results) {
    TestRandom r;
    r.state = 77;
    Half aux = MakeHalf(r);
    Half main = MakeHalf(r);
    MbBuild b;
    MbBuildText(results, "t.tsp", Program(aux, main), b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    a->lc.write_enable = true; // as a program's startup in the card leaves it
    tisple::RunApple2(*a, 400 * kMbTick);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, !a->sw.ramrd && !a->sw.ramwrt && !a->sw.altzp);
    CHECK(results, a->cpu.mem[0x03C4] < 0x80 && a->cpu.mem[0x03C5] < 0x80); // bank 1, the rom
    // the card's bytes, unpacked there
    bool card = true;
    for (size_t i = 0; i < aux.runs.size(); i++) {
        card = card && a->lc.aux_bank2_d000[i] == aux.runs[i];
    }
    for (size_t i = 0; i < main.runs.size(); i++) {
        card = card && a->lc.aux_e000[0x100 + i] == main.runs[i];
    }
    CHECK(results, card);
    // the backgrounds: medium blue on page 1, orange on page 2 (row 0 is untouched)
    uint8_t blue_aux[2] = {a->aux[0x2000], a->aux[0x2001]};
    uint8_t blue_main[2] = {a->cpu.mem[0x2000], a->cpu.mem[0x2001]};
    uint8_t orange_aux[2] = {a->aux[0x4000], a->aux[0x4001]};
    uint8_t orange_main[2] = {a->cpu.mem[0x4000], a->cpu.mem[0x4001]};
    CHECK(results, PageRight(*a, 0x2000, 4, 10, aux, main, false, blue_aux, blue_main));
    CHECK(results, PageRight(*a, 0x4000, 20, 60, aux, main, false, orange_aux, orange_main));
    CHECK(results, PageRight(*a, 0x2000, 26, 150, aux, main, true, blue_aux, blue_main));
    // no tick lost: interrupts are off while the card's code runs, but for a moment a row
    uint64_t counted = a->cpu.mem[kTicks] | (a->cpu.mem[kTicks + 1] << 8);
    uint64_t due = a->cpu.cycles / kMbTick;
    CHECK(results, counted == a->cpu.irqs_taken && counted + 2 >= due && counted <= due);
    std::printf("runs: %zu and %zu bytes of runs for %d bytes a memory, %llu ticks\n",
                aux.runs.size(), main.runs.size(), kW * kH,
                static_cast<unsigned long long>(counted));
}

} // namespace tisple_tests
