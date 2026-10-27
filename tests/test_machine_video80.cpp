// 80-column text, MouseText, double hi-res and double lo-res, pixel by pixel: the aux byte of
// each column shows first, then the main byte, 7 dots each

#include "machine/font.hpp"
#include "test_machine.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kBlack = 0x000000;

uint32_t PixelAt(const tisple::Framebuffer& fb, int x, int y) {
    return fb.pixels[y * tisple::kScreenWidth + x] & 0xFFFFFF;
}

tisple::Framebuffer Render(const tisple::Apple2& a, tisple::VideoLook look) {
    tisple::Framebuffer fb;
    tisple::RenderScreen(a, look, fb);
    return fb;
}

// a 7-dot glyph at half width: one pixel a dot, starting at x
bool HalfCellIs(const tisple::Framebuffer& fb, int row, int x, const tisple::TextCell& cell,
                bool inverse) {
    for (int r = 0; r < 8; r++) {
        uint8_t dots = tisple::CellRow(cell, r);
        for (int dot = 0; dot < 7; dot++) {
            bool on = (((dots >> dot) & 1) != 0) != inverse;
            if (PixelAt(fb, x + dot, row * 8 + r) != (on ? kWhite : kBlack)) {
                return false;
            }
        }
    }
    return true;
}

tisple::TextCell Letter(uint8_t ascii) {
    tisple::TextCell cell;
    cell.ascii = ascii;
    return cell;
}

void TestEightyColumns(TestResults& results) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    a->sw.col80 = true;
    a->aux[0x0400] = 0xC1;          // 'A' in column 0
    a->cpu.mem[0x0400] = 0xC2;      // 'B' in column 1
    a->aux[0x0401] = 0x03;          // inverse 'C' in column 2
    a->cpu.mem[0x07D0 + 39] = 0xE5; // 'e' in column 79 of row 23
    tisple::Framebuffer fb = Render(*a, tisple::VideoLook::kMono);
    CHECK(results, HalfCellIs(fb, 0, 0, Letter('A'), false));
    CHECK(results, HalfCellIs(fb, 0, 7, Letter('B'), false));
    CHECK(results, HalfCellIs(fb, 0, 14, Letter('C'), true));
    CHECK(results, HalfCellIs(fb, 23, 79 * 7, Letter('e'), false));
    // the flat look shows the same dots
    tisple::Framebuffer flat = Render(*a, tisple::VideoLook::kColor);
    CHECK(results, HalfCellIs(flat, 0, 0, Letter('A'), false));
    CHECK(results, HalfCellIs(flat, 0, 14, Letter('C'), true));

    // 40 columns again: the main byte alone, each dot two pixels wide
    a->sw.col80 = false;
    fb = Render(*a, tisple::VideoLook::kMono);
    uint8_t row0 = tisple::GlyphRow('B', 0);
    CHECK(results, PixelAt(fb, 2, 0) == (((row0 >> 1) & 1) != 0 ? kWhite : kBlack));
}

// the alternate set: MouseText at $40-$5F, inverse lower case at $60-$7F
void TestMouseText(TestResults& results) {
    tisple::TextCell apple = tisple::DecodeTextByte(0x40, true);
    CHECK(results, apple.mousetext && apple.ascii == 0);
    CHECK(results, tisple::DecodeTextByte(0x5F, true).mousetext);
    CHECK(results, !tisple::DecodeTextByte(0x60, true).mousetext);
    CHECK(results, !tisple::DecodeTextByte(0x40, false).mousetext);
    // every glyph is drawn: none is blank, and no two are the same
    int blank = 0;
    int same = 0;
    for (int g = 0; g < 32; g++) {
        uint64_t rows = 0;
        for (int r = 0; r < 8; r++) {
            rows |= static_cast<uint64_t>(tisple::MouseTextRow(static_cast<uint8_t>(g), r))
                    << (r * 8);
        }
        blank += rows == 0 ? 1 : 0;
        for (int h = 0; h < g; h++) {
            uint64_t other = 0;
            for (int r = 0; r < 8; r++) {
                other |= static_cast<uint64_t>(tisple::MouseTextRow(static_cast<uint8_t>(h), r))
                         << (r * 8);
            }
            same += other == rows ? 1 : 0;
        }
    }
    CHECK(results, blank == 0 && same == 0);

    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    a->sw.altchar = true;
    a->sw.col80 = true;
    a->cpu.mem[0x0400] = 0x4E; // the solid block, in column 1
    tisple::Framebuffer fb = Render(*a, tisple::VideoLook::kMono);
    CHECK(results, HalfCellIs(fb, 0, 7, tisple::DecodeTextByte(0x4E, true), false));
    CHECK(results, PixelAt(fb, 7, 0) == kWhite && PixelAt(fb, 13, 7) == kWhite);
}

// a line of double hi-res in one colour: dot k of the line is bit k mod 4 of the colour
void FillDoubleHiresLine(tisple::Apple2& a, int line, int color) {
    uint16_t base = static_cast<uint16_t>(0x2000 + (line & 7) * 0x400 + ((line >> 3) & 7) * 0x80 +
                                          (line >> 6) * 0x28);
    for (int col = 0; col < 40; col++) {
        uint8_t aux = 0;
        uint8_t main = 0;
        for (int bit = 0; bit < 7; bit++) {
            int dot = col * 14 + bit;
            aux = static_cast<uint8_t>(aux | (((color >> (dot & 3)) & 1) << bit));
            dot += 7;
            main = static_cast<uint8_t>(main | (((color >> (dot & 3)) & 1) << bit));
        }
        a.aux[base + col] = aux;
        a.cpu.mem[base + col] = main;
    }
}

void SetDoubleRes(tisple::Apple2& a, bool hires) {
    a.sw.text = false;
    a.sw.hires = hires;
    a.sw.col80 = true;
    a.sw.an[3] = false;
}

// the 16 colours match lo-res's, and a lone dot is one pixel of 560
void TestDoubleHires(TestResults& results) {
    std::unique_ptr<tisple::Apple2> lores = tisple::NewApple2();
    lores->sw.text = false;
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    SetDoubleRes(*a, true);
    for (int color = 0; color < 16; color++) {
        FillDoubleHiresLine(*a, color, color);
        lores->cpu.mem[0x0400 + 20] = static_cast<uint8_t>(color | (color << 4));
        tisple::Framebuffer want = Render(*lores, tisple::VideoLook::kColor);
        tisple::Framebuffer got = Render(*a, tisple::VideoLook::kColor);
        bool match = PixelAt(got, 281, color) == PixelAt(want, 281, 0);
        if (!match) {
            std::fprintf(stderr, "double hi-res colour %d: %06X, lo-res %06X\n", color,
                         PixelAt(got, 281, color), PixelAt(want, 281, 0));
        }
        CHECK(results, match);
    }
    std::unique_ptr<tisple::Apple2> dot = tisple::NewApple2();
    SetDoubleRes(*dot, true);
    dot->aux[0x2000] = 0x01;     // dot 0 of line 0
    dot->cpu.mem[0x2000] = 0x40; // dot 13
    tisple::Framebuffer fb = Render(*dot, tisple::VideoLook::kMono);
    CHECK(results, PixelAt(fb, 0, 0) == kWhite && PixelAt(fb, 1, 0) == kBlack);
    CHECK(results, PixelAt(fb, 13, 0) == kWhite && PixelAt(fb, 12, 0) == kBlack);
    // with an3 on again it is plain hi-res: the main byte only, two pixels a dot
    dot->sw.an[3] = true;
    fb = Render(*dot, tisple::VideoLook::kMono);
    CHECK(results, PixelAt(fb, 0, 0) == kBlack && PixelAt(fb, 12, 0) == kWhite);
}

// double lo-res: 80 blocks a row; aux nibbles are stored rotated left one bit
void TestDoubleLores(TestResults& results) {
    std::unique_ptr<tisple::Apple2> lores = tisple::NewApple2();
    lores->sw.text = false;
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    SetDoubleRes(*a, false);
    int bad = 0;
    for (int color = 0; color < 16; color++) {
        int rotated = ((color << 1) | (color >> 3)) & 15;
        a->aux[0x0400 + 10] = static_cast<uint8_t>(rotated);
        a->cpu.mem[0x0400 + 10] = static_cast<uint8_t>(color);
        lores->cpu.mem[0x0400 + 10] = static_cast<uint8_t>(color);
        tisple::Framebuffer want = Render(*lores, tisple::VideoLook::kColor);
        tisple::Framebuffer got = Render(*a, tisple::VideoLook::kColor);
        uint32_t expect = PixelAt(want, 140, 0);
        bad += PixelAt(got, 140, 0) != expect || PixelAt(got, 147, 0) != expect ? 1 : 0;
        // the ntsc look draws the same dot pattern in both halves
        tisple::Framebuffer ntsc_lores = Render(*lores, tisple::VideoLook::kMono);
        tisple::Framebuffer ntsc = Render(*a, tisple::VideoLook::kMono);
        for (int x = 140; x < 154; x++) {
            bad += PixelAt(ntsc, x, 0) != PixelAt(ntsc_lores, x, 0) ? 1 : 0;
        }
    }
    CHECK(results, bad == 0);
}

// the picture follows the memory switches: 80STORE keeps page 1 on screen
void TestStorePageDisplay(TestResults& results) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    a->cpu.mem[0x0400] = 0xC1;
    a->cpu.mem[0x0800] = 0xC2;
    a->sw.page2 = true;
    a->sw.store80 = true;
    tisple::VideoFrame frame;
    tisple::CaptureScreen(*a, frame);
    CHECK(results, frame.bytes[0][0] == 0xC1);
    a->sw.store80 = false;
    tisple::CaptureScreen(*a, frame);
    CHECK(results, frame.bytes[0][0] == 0xC2);
}

} // namespace

void RunMachineVideo80Tests(TestResults& results) {
    TestEightyColumns(results);
    TestMouseText(results);
    TestDoubleHires(results);
    TestDoubleLores(results);
    TestStorePageDisplay(results);
}

} // namespace tisple_tests
