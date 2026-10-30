// lib/dhgr on the built-in IIe: the switches it sets, each drawing routine on each page and
// memory read back as pixels or bytes, against what double hi-res shows (a pixel is 4 dots, the
// aux byte's 7 dots before the main byte's in each column)

#include "test_mockingboard.hpp"
#include "test_pack.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

const char* kProgram = R"(
(import dhgr)
(import dhgr_text2)
(import tables)
(macro pic_byte (i) (% (* i 29) 127))
(macro mask_byte (i) (if (= (% i 3) 0) 0x7F 0x0F))
(tables.table PIC_MAIN u8 24 pic_byte)
(tables.table MASK_MAIN u8 24 mask_byte)
(macro aux_pic () `(make (array u8 24) ,@(map (fn (i) (pic_byte (+ i 5))) (range 0 24))))
(macro aux_mask () `(make (array u8 24) ,@(map mask_byte (range 0 24))))
(let pic_aux (array u8 24) (aux_pic) :in aux)
(let mask_aux (array u8 24) (aux_mask) :in aux)
(let codes (array u8 80))
(let old u8 0 :address 0x03C0)
(fn main () void
    (dhgr.clear dhgr.PAGE1 dhgr.MEDIUM_BLUE)
    (dhgr.clear dhgr.PAGE2 dhgr.BLACK)
    (dhgr.box dhgr.PAGE1 4 10 6 20 dhgr.ORANGE)
    (dhgr.box dhgr.PAGE2 5 100 7 9 dhgr.PINK)         ; odd column, odd width
    (for x u8 0 140
        (dhgr.plot dhgr.PAGE1 x 60 (& x 15))
        (dhgr.plot dhgr.PAGE2 x 191 (& (+ x 3) 15)))
    (dhgr.plot dhgr.PAGE1 139 0 dhgr.WHITE)
    ; sprites: 4 bytes a row, 6 rows, at column 20 row 70
    (dhgr.masked dhgr.PAGE1 dhgr.MAIN 20 70 4 6 (addr_of (at PIC_MAIN 0)) (addr_of (at MASK_MAIN 0)))
    (dhgr.masked dhgr.PAGE1 dhgr.AUX 20 70 4 6 (addr_of (at pic_aux 0)) (addr_of (at mask_aux 0)))
    (dhgr.copy dhgr.PAGE2 dhgr.MAIN 30 4 4 6 (addr_of (at PIC_MAIN 0)))
    (dhgr.copy dhgr.PAGE2 dhgr.AUX 30 4 4 6 (addr_of (at pic_aux 0)))
    ; page 2's pink box onto page 1, rows across a band edge
    (dhgr.copy_box dhgr.PAGE1 dhgr.MAIN 5 100 7 9 dhgr.PAGE2)
    (dhgr.copy_box dhgr.PAGE1 dhgr.AUX 5 100 7 9 dhgr.PAGE2)
    (dhgr.fill dhgr.PAGE2 dhgr.AUX 9 150 3 2 0x11 0x22)
    (set old (dhgr.aux_rmw 0x2000 0xF0 0x05))
    (for i u8 0 80
        (set (at codes i) (+ 0xC1 (% i 26))))
    (dhgr.text_row dhgr.PAGE1 20 (addr_of (at codes 0)))
    (set (at codes 0) 0xAA)
    (dhgr.text_row dhgr.PAGE2 23 (addr_of (at codes 0)))
    (dhgr.on true)
    (dhgr.show dhgr.PAGE2))
)";

int RowAddress(int page, int y) {
    return page + (y & 7) * 0x400 + ((y >> 3) & 7) * 0x80 + (y >> 6) * 0x28;
}

// the colour double hi-res shows for pixel x of row y: its 4 dots as a lo-res number
int Pixel(const tisple::Apple2& a, int page, int x, int y) {
    int colour = 0;
    for (int k = 0; k < 4; k++) {
        int dot = x * 4 + k;
        int col = dot / 14;
        int bit = dot % 14;
        uint8_t b =
            bit < 7 ? a.aux[RowAddress(page, y) + col] : a.cpu.mem[RowAddress(page, y) + col];
        colour |= ((b >> (bit % 7)) & 1) << k;
    }
    return colour;
}

// pixels whose 4 dots all lie in byte columns col to col + w - 1
bool InColumns(int x, int col, int w) {
    return x * 4 >= col * 14 && x * 4 + 3 < (col + w) * 14;
}

int Mod(int a, int m) {
    return ((a % m) + m) % m;
}

void PixelTests(TestResults& results, const tisple::Apple2& a) {
    const int p1 = 0x2000;
    const int p2 = 0x4000;
    bool clear_ok = true;
    bool box_ok = true;
    for (int y = 1; y < 192; y += 3) { // row 0 has the corner pixel and aux_rmw's byte
        for (int x = 0; x < 139; x++) {
            bool in_box = y >= 10 && y < 30 && InColumns(x, 4, 6);
            bool touches = y >= 10 && y < 30 && x * 4 + 3 >= 4 * 14 && x * 4 < 10 * 14;
            bool sprite = y >= 70 && y < 76 && x * 4 + 3 >= 20 * 14 && x * 4 < 24 * 14;
            bool copied = y >= 100 && y < 109 && x * 4 + 3 >= 5 * 14 && x * 4 < 12 * 14;
            int got = Pixel(a, p1, x, y);
            if (in_box) {
                box_ok = box_ok && got == 9;
            } else if (!touches && !sprite && !copied && y != 60) {
                clear_ok = clear_ok && got == 6;
            }
        }
    }
    CHECK(results, clear_ok && box_ok);
    // every place in a 7-pixel group, every colour, both pages; the corner pixel
    bool plots = true;
    for (int x = 0; x < 140; x++) {
        plots = plots && Pixel(a, p1, x, 60) == (x & 15);
        plots = plots && Pixel(a, p2, x, 191) == ((x + 3) & 15);
    }
    CHECK(results, plots);
    CHECK(results, Pixel(a, p1, 139, 0) == 15 && Pixel(a, p1, 138, 0) == 6);
    // page 2's box (odd column, odd width), and the same pixels copied onto page 1
    bool pink = true;
    for (int y = 100; y < 109; y++) {
        for (int x = 0; x < 140; x++) {
            if (InColumns(x, 5, 7)) {
                pink = pink && Pixel(a, p2, x, y) == 11 && Pixel(a, p1, x, y) == 11;
            }
        }
    }
    CHECK(results, pink);
}

void ByteTests(TestResults& results, const tisple::Apple2& a) {
    const int p1 = 0x2000;
    const int p2 = 0x4000;
    // the masked sprite over the blue: (screen & mask) | pic, main and aux, a row at a time
    uint8_t blue_main[2] = {a.cpu.mem[p1 + 2], a.cpu.mem[p1 + 3]}; // row 0 is all blue still
    uint8_t blue_aux[2] = {a.aux[p1 + 2], a.aux[p1 + 3]};
    bool masked = true;
    bool copied = true;
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c < 4; c++) {
            int i = r * 4 + c;
            int at = RowAddress(p1, 70 + r) + 20 + c;
            uint8_t mask = i % 3 == 0 ? 0x7F : 0x0F;
            int want_main = (blue_main[c & 1] & mask) | ((i * 29) % 127);
            int want_aux = (blue_aux[c & 1] & mask) | (((i + 5) * 29) % 127);
            masked = masked && a.cpu.mem[at] == want_main && a.aux[at] == want_aux;
            int at2 = RowAddress(p2, 4 + r) + 30 + c;
            copied =
                copied && a.cpu.mem[at2] == (i * 29) % 127 && a.aux[at2] == ((i + 5) * 29) % 127;
        }
    }
    CHECK(results, masked && copied);
    // fill from an odd column: 0x11 in even columns 10, 0x22 in 9 and 11; main untouched
    bool fill = true;
    for (int r = 150; r < 152; r++) {
        int at = RowAddress(p2, r);
        fill = fill && a.aux[at + 9] == 0x22 && a.aux[at + 10] == 0x11 && a.aux[at + 11] == 0x22;
        fill = fill && a.aux[at + 8] == 0 && a.aux[at + 12] == 0 && a.cpu.mem[at + 9] == 0;
    }
    CHECK(results, fill);
    // aux_rmw on the blue: gave the old byte, kept the high nibble, set 5
    CHECK(results, a.cpu.mem[0x03C0] == blue_aux[0] && a.aux[p1] == ((blue_aux[0] & 0xF0) | 5));
    // text rows: even columns in aux memory, odd in main; page 2's row on text page 2
    int row20 = 0x0400 + (20 % 8) * 0x80 + (20 / 8) * 40;
    int row23 = 0x0800 + (23 % 8) * 0x80 + (23 / 8) * 40;
    bool text = true;
    for (int i = 0; i < 40; i++) {
        text = text && a.aux[row20 + i] == 0xC1 + Mod(2 * i, 26) &&
               a.cpu.mem[row20 + i] == 0xC1 + Mod(2 * i + 1, 26);
        uint8_t first = i == 0 ? 0xAA : static_cast<uint8_t>(0xC1 + Mod(2 * i, 26));
        text =
            text && a.aux[row23 + i] == first && a.cpu.mem[row23 + i] == 0xC1 + Mod(2 * i + 1, 26);
    }
    CHECK(results, text);
}

} // namespace

void RunDhgrLibTests(TestResults& results) {
    MbBuild b;
    MbBuildText(results, "t.tsp", kProgram, b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 0);
    tisple::RunApple2(*a, 20 * 1000 * 1000);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    // double hi-res, mixed, page 2, 80 columns, 80STORE off, the banks back on main
    const tisple::SoftSwitches& sw = a->sw;
    CHECK(results, !sw.text && sw.mixed && sw.hires && !sw.an[3] && sw.col80);
    CHECK(results, sw.page2 && !sw.store80 && !sw.ramrd && !sw.ramwrt);
    PixelTests(results, *a);
    ByteTests(results, *a);
}

} // namespace tisple_tests
