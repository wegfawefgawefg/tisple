// lib/dlores, the mixed modes (lib/lores, lib/hgr, lib/dlores with lib/text40 and lib/text80's
// window) and lib/mousetext on the built-in machine: what lands in which memory, the switches,
// and what the screen shows

#include "test_apple2_libs.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

std::unique_ptr<tisple::Apple2> RunLib(TestResults& results, const std::string& text) {
    LibBuild b;
    if (!BuildLibProgram(results, text, tisple::BuildOptions(), b)) {
        return nullptr;
    }
    std::unique_ptr<tisple::Apple2> a = LoadLibProgram(b);
    if (!RunToReturn(results, *a, 20000000)) {
        return nullptr;
    }
    return a;
}

int RowBase(int r) {
    return 0x0400 + (r % 8) * 0x80 + (r / 8) * 40;
}

// the screen's colour at a 560 x 192 dot
uint32_t Dot(const tisple::Framebuffer& fb, int x, int y) {
    return fb.pixels[static_cast<size_t>(y * tisple::kScreenWidth + x)] & 0xFFFFFF;
}

// every colour in an even (aux) and an odd (main) column, rows 0-1 and 2-3: the same colour on
// the screen either way, the colours the plain lo-res screen shows, and get gives them back
void TestDloresColours(TestResults& results) {
    auto a = RunLib(results, "(import dlores)\n"
                             "(let out (array u8 64) :address 0x0300)\n"
                             "(fn main () void\n"
                             "    (dlores.on false)\n"
                             "    (dlores.clear dlores.BLACK)\n"
                             "    (for c u8 0 16\n"
                             "        (dlores.plot (* c 4) 0 c)\n"
                             "        (dlores.plot (+ (* c 4) 1) 0 c)\n"
                             "        (dlores.plot (* c 4) 3 c)\n"
                             "        (dlores.plot (+ (* c 4) 1) 3 c))\n"
                             "    (for c u8 0 16\n"
                             "        (set (at out c) (dlores.get (* c 4) 0))\n"
                             "        (set (at out (+ c 16)) (dlores.get (+ (* c 4) 1) 0))\n"
                             "        (set (at out (+ c 32)) (dlores.get (* c 4) 3))\n"
                             "        (set (at out (+ c 48)) (dlores.get (+ (* c 4) 1) 3))))\n");
    if (a == nullptr) {
        return;
    }
    CHECK(results, a->sw.col80 && !a->sw.an[3] && !a->sw.text && !a->sw.hires && !a->sw.mixed);
    bool gets = true;
    for (int c = 0; c < 16; c++) {
        for (int k = 0; k < 4; k++) {
            gets = gets && a->cpu.mem[0x0300 + c + 16 * k] == c;
        }
    }
    CHECK(results, gets);
    // aux holds the colour rotated left, main as it is; the low nibble is row 0
    CHECK(results, (a->aux[0x0400 + 2] & 0x0F) == 0x02 && (a->cpu.mem[0x0400 + 2] & 0x0F) == 0x01);
    CHECK(results, (a->aux[0x0400 + 4] & 0x0F) == 0x04 && (a->cpu.mem[0x0400 + 4] & 0x0F) == 0x02);
    tisple::Framebuffer fb;
    tisple::RenderScreen(*a, tisple::VideoLook::kColor, fb);
    // the same colours on a plain lo-res screen
    auto b = RunLib(results, "(import lores)\n"
                             "(fn main () void\n"
                             "    (lores.show)\n"
                             "    (for c u8 0 16\n"
                             "        (lores.plot (* c 2) 0 c)))\n");
    if (b == nullptr) {
        return;
    }
    tisple::Framebuffer lo;
    tisple::RenderScreen(*b, tisple::VideoLook::kColor, lo);
    bool same = true;
    for (int c = 0; c < 16; c++) {
        // block x is 7 dots wide in double lo-res, 14 in lo-res; the middles, row 2 of 4
        uint32_t even = Dot(fb, c * 4 * 7 + 3, 1);
        uint32_t odd = Dot(fb, (c * 4 + 1) * 7 + 3, 1);
        uint32_t lower = Dot(fb, c * 4 * 7 + 3, 13);
        uint32_t want = Dot(lo, c * 2 * 14 + 7, 1);
        if (even != want || odd != want || lower != want) {
            std::fprintf(stderr, "dlores colour %d: %06X %06X %06X, lo-res %06X\n", c, even, odd,
                         lower, want);
            same = false;
        }
    }
    CHECK(results, same);
}

// mixed modes: the graphics' clear leaves the text rows, the text's window leaves the graphics
void TestMixed(TestResults& results) {
    auto a = RunLib(results, "(import lores)\n"
                             "(import text40)\n"
                             "(fn main () void\n"
                             "    (lores.mixed)\n"
                             "    (text40.window 20)\n"
                             "    (text40.clear)\n"
                             "    (text40.print \"A\")\n"
                             "    (lores.clear)\n"
                             "    (lores.plot 5 39 lores.WHITE)\n"
                             "    (for i u8 0 6\n"
                             "        (text40.put 13)\n"
                             "        (text40.put (+ 'B' i))))\n");
    if (a != nullptr) {
        CHECK(results, !a->sw.text && a->sw.mixed && !a->sw.hires);
        CHECK(results, a->cpu.mem[RowBase(19) + 5] == 0xF0); // block 39, the lower nibble
        // six lines in a window of four: B and C scrolled off, row 19 left alone
        CHECK(results, a->cpu.mem[RowBase(20)] == 0xC4 && a->cpu.mem[RowBase(23)] == 0xC7);
        CHECK(results, a->cpu.mem[RowBase(19)] == 0x00);
        tisple::VideoFrame frame;
        tisple::CaptureScreen(*a, frame);
        CHECK(results, (frame.modes[159][0] & 0x07) == tisple::kVideoLores &&
                           (frame.modes[160][0] & 0x07) == tisple::kVideoText);
    }
    auto h = RunLib(results, "(import hgr)\n"
                             "(fn main () void\n"
                             "    (hgr.mixed))\n");
    if (h != nullptr) {
        CHECK(results, !h->sw.text && h->sw.mixed && h->sw.hires && !h->sw.page2);
    }
    // double lo-res over 80-column text: text80's window, both halves
    auto d = RunLib(results, "(import dlores)\n"
                             "(import text80)\n"
                             "(fn main () void\n"
                             "    (dlores.on true)\n"
                             "    (dlores.clear dlores.ORANGE)\n"
                             "    (text80.window 20)\n"
                             "    (text80.clear)\n"
                             "    (text80.print \"HI\"))\n");
    if (d != nullptr) {
        CHECK(results, d->sw.mixed && d->sw.col80 && !d->sw.an[3] && d->sw.store80);
        CHECK(results, d->cpu.mem[RowBase(19)] == 0x99 && d->aux[RowBase(19)] == 0x33);
        CHECK(results, d->aux[RowBase(20)] == 0xC8 && d->cpu.mem[RowBase(20)] == 0xC9);
        CHECK(results, d->aux[RowBase(21)] == 0xA0);
        tisple::VideoFrame frame;
        tisple::CaptureScreen(*d, frame);
        CHECK(results, frame.modes[100][3] == (tisple::kVideoLores | tisple::kVideoDouble));
        CHECK(results, (frame.modes[170][3] & tisple::kVideoDouble) != 0 &&
                           (frame.modes[170][3] & 0x07) == tisple::kVideoText);
    }
}

// a frame round a window of text, in 80 columns and 40
void TestMouseText(TestResults& results) {
    auto a = RunLib(results, "(import mousetext)\n"
                             "(import text80)\n"
                             "(fn main () void\n"
                             "    (text80.on)\n"
                             "    (mousetext.frame80 10 5 4 2)\n"
                             "    (text80.move 10 5)\n"
                             "    (text80.put_code mousetext.CHECK))\n");
    if (a != nullptr) {
        auto cell = [&](int c, int r) {
            int at = RowBase(r) + c / 2;
            return (c % 2) == 0 ? a->aux[at] : a->cpu.mem[at];
        };
        CHECK(results, a->sw.altchar);
        CHECK(results, cell(10, 4) == 0xDF && cell(13, 4) == 0xDF && cell(14, 4) == 0xA0);
        CHECK(results, cell(10, 7) == 0x4C && cell(13, 7) == 0x4C);
        CHECK(results, cell(9, 5) == 0x5A && cell(9, 6) == 0x5A && cell(14, 6) == 0x5F);
        CHECK(results, cell(10, 5) == 0x44);
    }
    auto b = RunLib(results, "(import mousetext)\n"
                             "(import text40)\n"
                             "(fn main () void\n"
                             "    (text40.on)\n"
                             "    (mousetext.on)\n"
                             "    (mousetext.frame40 1 1 3 1)\n"
                             "    (mousetext.bar40 10))\n");
    if (b != nullptr) {
        const uint8_t* m = b->cpu.mem;
        CHECK(results, b->sw.altchar && !b->sw.col80);
        CHECK(results, m[RowBase(0) + 1] == 0xDF && m[RowBase(2) + 3] == 0x4C);
        CHECK(results, m[RowBase(1)] == 0x5A && m[RowBase(1) + 4] == 0x5F);
        CHECK(results, m[RowBase(10)] == 0x4C && m[RowBase(10) + 39] == 0x4C);
    }
}

} // namespace

void RunVideoModeLibTests(TestResults& results) {
    TestDloresColours(results);
    TestMixed(results);
    TestMouseText(results);
}

} // namespace tisple_tests
