// video of the built-in apple iie: the test programs in tests/programs/machine_*.bin
// (scripts/make_machine_programs.py) drawn and checked pixel by pixel, plus checksums of
// the whole frame so any change to the renderer shows

#include "machine/font.hpp"
#include "machine/png.hpp"
#include "test_machine.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kBlack = 0x000000;

uint32_t Pixel(const tisple::Framebuffer& fb, int x, int y) {
    return fb.pixels[y * tisple::kScreenWidth + x];
}

int Red(uint32_t p) {
    return (p >> 16) & 0xFF;
}

int Green(uint32_t p) {
    return (p >> 8) & 0xFF;
}

int Blue(uint32_t p) {
    return p & 0xFF;
}

// the average of four pixels: one cycle of the color carrier
uint32_t Average4(const tisple::Framebuffer& fb, int x, int y) {
    int r = 0;
    int g = 0;
    int b = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t p = Pixel(fb, x + i, y);
        r += Red(p);
        g += Green(p);
        b += Blue(p);
    }
    return static_cast<uint32_t>(((r / 4) << 16) | ((g / 4) << 8) | (b / 4));
}

std::unique_ptr<tisple::Apple2> Drawn(const char* name) {
    std::unique_ptr<tisple::Apple2> a = MachineWithFile(name);
    if (a != nullptr) {
        RunToStop(*a);
    }
    return a;
}

void CheckChecksum(TestResults& results, const char* what, const tisple::Framebuffer& fb,
                   uint32_t expected) {
    uint32_t got = tisple::FramebufferChecksum(fb);
    if (got != expected) {
        std::fprintf(stderr, "%s: framebuffer checksum %08X, expected %08X\n", what, got, expected);
    }
    CHECK(results, got == expected);
}

// one 7x8 character cell in a mono-rendered frame
bool CellIs(const tisple::Framebuffer& fb, int row, int col, uint8_t ascii, bool inverse) {
    for (int r = 0; r < 8; r++) {
        uint8_t dots = tisple::GlyphRow(ascii, r);
        for (int dot = 0; dot < 7; dot++) {
            bool on = (((dots >> dot) & 1) != 0) != inverse;
            uint32_t want = on ? kWhite : kBlack;
            int x = col * 14 + dot * 2;
            int y = row * 8 + r;
            if (Pixel(fb, x, y) != want || Pixel(fb, x + 1, y) != want) {
                return false;
            }
        }
    }
    return true;
}

void TestText(TestResults& results) {
    auto a = Drawn("machine_text.bin");
    if (a == nullptr) {
        CHECK(results, false);
        return;
    }
    tisple::Framebuffer fb;
    tisple::RenderScreen(*a, tisple::VideoLook::kNtsc, fb);
    // byte X sits at $400 + X: $C1 at row 9 col 25, $01 at row 0 col 1, $41 at row 8 col 25
    CHECK(results, CellIs(fb, 9, 25, 'A', false));
    CHECK(results, CellIs(fb, 0, 1, 'A', true));
    CHECK(results, CellIs(fb, 8, 25, 'A', false));  // flashing, in its normal phase
    CHECK(results, CellIs(fb, 17, 17, 'a', false)); // $E1
    CHECK(results, CellIs(fb, 23, 0, 'T', false));  // the message on row 23
    CheckChecksum(results, "text", fb, 0xB787BB8D);

    tisple::VideoFrame frame;
    tisple::CaptureScreen(*a, frame);
    frame.flash_inverse = true;
    tisple::RenderFrame(frame, tisple::VideoLook::kNtsc, fb);
    CHECK(results, CellIs(fb, 8, 25, 'A', true));
    CHECK(results, CellIs(fb, 9, 25, 'A', false));

    // the alternate set of the unenhanced iie: inverse lower case instead of flashing
    tisple::TextCell flash = tisple::DecodeTextByte(0x61, false);
    tisple::TextCell alt = tisple::DecodeTextByte(0x61, true);
    CHECK(results, flash.ascii == '!' && flash.style == tisple::TextStyle::kFlash);
    CHECK(results, alt.ascii == 'a' && alt.style == tisple::TextStyle::kInverse);
    CHECK(results, tisple::DecodeTextByte(0x1B, false).ascii == '[');
    CHECK(results, tisple::DecodeTextByte(0xA0, false).ascii == ' ');
}

// the flash clock: 16 frames each way
void TestFlashRate(TestResults& results) {
    auto a = MachineWithBytes({0x4C, 0x00, 0x60});
    tisple::RunApple2(*a, 15 * tisple::kCyclesPerFrame + tisple::kVblankStartCycle + 10);
    CHECK(results, a->video.done.frame_index == 15 && !a->video.done.flash_inverse);
    tisple::RunApple2(*a, tisple::kCyclesPerFrame);
    CHECK(results, a->video.done.frame_index == 16 && a->video.done.flash_inverse);
    CHECK(results, a->video.frames_done == 17);
}

void TestLores(TestResults& results) {
    auto a = Drawn("machine_lores.bin");
    if (a == nullptr) {
        CHECK(results, false);
        return;
    }
    tisple::Framebuffer fb;
    tisple::RenderScreen(*a, tisple::VideoLook::kNtsc, fb);
    // column x is color x mod 16. a pixel's color depends on the last 12 dots, so only
    // pixels 9-11 of a 14-dot block see that block alone (the filter lags two dots)
    bool steady = true;
    for (int y = 0; y < tisple::kScreenHeight; y += 5) {
        for (int col = 0; col < 24; col++) {
            for (int x = col * 14 + 9; x <= col * 14 + 11; x++) {
                // the same color sixteen columns on looks the same
                steady = steady && Pixel(fb, x, y) == Pixel(fb, x + 16 * 14, y);
            }
        }
    }
    CHECK(results, steady);
    CHECK(results, Pixel(fb, 10, 10) == kBlack);
    CHECK(results, Pixel(fb, 15 * 14 + 10, 10) == kWhite);
    uint32_t grey = Pixel(fb, 5 * 14 + 10, 10);
    CHECK(results, grey == 0x838383 || grey == 0x787878);
    uint32_t orange = Average4(fb, 9 * 14 + 9, 10); // color 9
    CHECK(results, Red(orange) > Green(orange) && Red(orange) > Blue(orange));
    uint32_t green = Average4(fb, 12 * 14 + 9, 10); // color 12
    CHECK(results, Green(green) > Red(green) && Green(green) > Blue(green));
    uint32_t blue = Average4(fb, 6 * 14 + 9, 10); // color 6, medium blue
    CHECK(results, Blue(blue) > Red(blue));
    CheckChecksum(results, "lores", fb, 0x331B77C5);
}

// hi-res byte at (line, col) of page 1, from the plain layout
uint8_t HiresByte(const tisple::Apple2& a, int line, int col, int page) {
    int row = line / 8;
    int addr = 0x2000 * page + (line & 7) * 0x400 + (row & 7) * 0x80 + (row >> 3) * 0x28 + col;
    return a.cpu.mem[addr];
}

// monochrome hi-res, stated directly: each bit is two dots; bit 7 delays the byte by one
// dot, which repeats the previous byte's last dot
bool MonoHiresMatches(const tisple::Apple2& a, const tisple::Framebuffer& fb, int page,
                      int first_line, int end_line) {
    for (int y = first_line; y < end_line; y++) {
        int last_dot = 0;
        for (int col = 0; col < 40; col++) {
            uint8_t b = HiresByte(a, y, col, page);
            int dots[14];
            for (int d = 0; d < 14; d++) {
                if ((b & 0x80) == 0) {
                    dots[d] = (b >> (d / 2)) & 1;
                } else {
                    dots[d] = d == 0 ? last_dot : (b >> ((d - 1) / 2)) & 1;
                }
            }
            for (int d = 0; d < 14; d++) {
                if (Pixel(fb, col * 14 + d, y) != (dots[d] != 0 ? kWhite : kBlack)) {
                    return false;
                }
            }
            last_dot = dots[13];
        }
    }
    return true;
}

void TestHires(TestResults& results) {
    auto a = Drawn("machine_hires.bin");
    if (a == nullptr) {
        CHECK(results, false);
        return;
    }
    tisple::Framebuffer fb;
    tisple::RenderScreen(*a, tisple::VideoLook::kMono, fb);
    CHECK(results, MonoHiresMatches(*a, fb, 1, 0, 192));
    CheckChecksum(results, "hires mono", fb, 0xF11A29C5);

    tisple::RenderScreen(*a, tisple::VideoLook::kNtsc, fb);
    // stripes of eight columns: white, violet, green, blue, orange
    bool white = true;
    bool steady = true;
    for (int y = 0; y < tisple::kScreenHeight; y += 3) {
        for (int x = 4; x < 8 * 14 - 4; x++) {
            white = white && Pixel(fb, x, y) == kWhite;
        }
        for (int stripe = 1; stripe < 5; stripe++) {
            // away from the edges, where the 12-dot window sees the neighbouring stripe
            for (int x = stripe * 112 + 16; x < stripe * 112 + 92; x++) {
                steady = steady && Pixel(fb, x, y) == Pixel(fb, x + 4, y);
            }
        }
    }
    CHECK(results, white);
    CHECK(results, steady);
    uint32_t violet = Average4(fb, 112 + 40, 50);
    uint32_t green = Average4(fb, 224 + 40, 50);
    uint32_t blue = Average4(fb, 336 + 40, 50);
    uint32_t orange = Average4(fb, 448 + 40, 50);
    CHECK(results, Red(violet) > Green(violet) && Blue(violet) > Green(violet));
    CHECK(results, Green(green) > Red(green) && Green(green) > Blue(green));
    CHECK(results, Blue(blue) > Red(blue) && Blue(blue) > Green(blue) / 2);
    CHECK(results, Red(orange) > Blue(orange) && Red(orange) > Green(orange));
    CheckChecksum(results, "hires color", fb, 0xC2830045);
}

void TestMixed(TestResults& results) {
    auto a = Drawn("machine_mixed.bin");
    if (a == nullptr) {
        CHECK(results, false);
        return;
    }
    tisple::VideoFrame frame;
    tisple::CaptureScreen(*a, frame);
    CHECK(results, frame.modes[159][0] == tisple::kVideoHires);
    CHECK(results, frame.modes[160][0] == tisple::kVideoText);
    CHECK(results, frame.burst[170]); // the color burst stays on: text gets color fringes
    tisple::Framebuffer fb;
    tisple::RenderScreen(*a, tisple::VideoLook::kMono, fb);
    CHECK(results, MonoHiresMatches(*a, fb, 1, 0, 160));
    CHECK(results, CellIs(fb, 20, 0, 'M', false));
    CHECK(results, CellIs(fb, 21, 0, ' ', false));
    tisple::RenderScreen(*a, tisple::VideoLook::kNtsc, fb);
    CheckChecksum(results, "mixed", fb, 0x79BEA1FD);
}

void TestPage2(TestResults& results) {
    auto a = Drawn("machine_page2.bin");
    if (a == nullptr) {
        CHECK(results, false);
        return;
    }
    CHECK(results, a->sw.page2);
    tisple::Framebuffer fb;
    tisple::RenderScreen(*a, tisple::VideoLook::kMono, fb);
    CHECK(results, MonoHiresMatches(*a, fb, 2, 0, 192));
    CHECK(results, Pixel(fb, 7, 100) == kWhite);          // column 0
    CHECK(results, Pixel(fb, 2 * 14 + 7, 100) == kBlack); // column 2
    tisple::RenderScreen(*a, tisple::VideoLook::kNtsc, fb);
    CheckChecksum(results, "page2", fb, 0x1D8747C5);
}

// a mode switch mid-frame shows in the frame the beam drew, from the exact column
void TestMidFrameSwitch(TestResults& results) {
    // LDX #255 ; DEX ; BNE ; STA $C050 (write at cycle 5 * 255 + 4 = 1279) ; JMP *
    auto a = MachineWithBytes({0xA2, 0xFF, 0xCA, 0xD0, 0xFD, 0x8D, 0x50, 0xC0, 0x4C, 0x08, 0x60});
    tisple::RunApple2(*a, tisple::kCyclesPerFrame + 100);
    // cycle 1279 is line 19, cycle 44 of the line: column 19 still text, 20 graphics
    const tisple::VideoFrame& f = a->video.done;
    CHECK(results, f.frame_index == 0);
    CHECK(results, f.modes[18][39] == tisple::kVideoText);
    CHECK(results, f.modes[19][19] == tisple::kVideoText);
    CHECK(results, f.modes[19][20] == tisple::kVideoLores);
    CHECK(results, f.modes[100][0] == tisple::kVideoLores);
    CHECK(results, !f.burst[19] && f.burst[20]);
}

} // namespace

void RunMachineVideoTests(TestResults& results) {
    TestText(results);
    TestFlashRate(results);
    TestLores(results);
    TestHires(results);
    TestMixed(results);
    TestPage2(results);
    TestMidFrameSwitch(results);
}

} // namespace tisple_tests
