// scanner fetches to pixels, through an ntsc model.
//
// ported from AppleWin's NTSC.cpp (GPL-2.0-or-later, see third_party/applewin; by William S
// Simms, Michael Pohoreski and Tom Charlesworth): every mode becomes a stream of 14 MHz dots,
// 560 per line. a 12-dot shift register of the latest dots, with the color phase (dot index
// mod 4), indexes a table built by filtering each 12-dot sequence and decoding it as yiq.
// that gives hi-res artifact color, the bit 7 half-dot shift, lo-res colors and color
// fringes on text in mixed mode, as AppleWin's "color monitor ntsc" shows them. lines
// without color burst (all-text screens) and the monochrome option use the plain dots.
//
// the ntsc table carries filter state from one entry to the next (as AppleWin does), so a
// steady color comes out a little different at each of the 4 phases: fine stripes every 4
// dots, and colors smear for a few dots where patterns meet. the flat look skips the signal
// altogether, like an rgb monitor: lo-res blocks get their color outright, hi-res follows
// the usual idealized rules, and text is plain white. its colors are the ntsc table's own,
// averaged over the 4 phases, so the looks agree on what each color is.
// text glyphs come from our own font (font.cpp), not Apple's character rom.

#include "machine/apple2.hpp"
#include "machine/font.hpp"

#include <array>
#include <cmath>

namespace tisple {

namespace {

constexpr int kPhases = 4;
constexpr int kSequences = 4096;

struct NtscTables {
    uint32_t hue[kPhases][kSequences];
    uint32_t mono[kSequences];
    uint32_t flat[kPhases][16]; // steady color by phase and the last 4 dots
    uint32_t lores[16];         // by nibble
    uint32_t hires[2][2];       // a lone on pixel, by palette bit and column parity
};

// a second-order iir section; AppleWin keeps these as statics, so their state runs on from
// one table entry to the next, and so does ours
struct Filter {
    double x[3] = {};
    double y[3] = {};
};

double FilterSignal(Filter& f, double z) {
    f.x[0] = f.x[1];
    f.x[1] = f.x[2];
    f.x[2] = z / 7.614490548;
    f.y[0] = f.y[1];
    f.y[1] = f.y[2];
    f.y[2] = f.x[0] + f.x[2] + 2.0 * f.x[1] + (-0.2718798058 * f.y[0]) + (0.7465656072 * f.y[1]);
    return f.y[2];
}

double FilterChroma(Filter& f, double z) {
    f.x[0] = f.x[1];
    f.x[1] = f.x[2];
    f.x[2] = z / 7.438011255;
    f.y[0] = f.y[1];
    f.y[1] = f.y[2];
    f.y[2] = -f.x[0] + f.x[2] + (-0.7318893645 * f.y[0]) + (1.2336442711 * f.y[1]);
    return f.y[2];
}

double FilterLuma(Filter& f, double z) {
    f.x[0] = f.x[1];
    f.x[1] = f.x[2];
    f.x[2] = z / 13.71331570;
    f.y[0] = f.y[1];
    f.y[1] = f.y[2];
    f.y[2] = f.x[0] + f.x[2] + 2.0 * f.x[1] + (-0.3961075449 * f.y[0]) + (1.1044202472 * f.y[1]);
    return f.y[2];
}

float ClampUnit(double v) {
    float f = static_cast<float>(v);
    if (f < 0.f) {
        return 0.f;
    }
    return f > 1.f ? 1.f : f;
}

uint32_t Rgb(float r, float g, float b) {
    auto channel = [](float v) { return static_cast<uint32_t>(static_cast<uint8_t>(v * 255)); };
    return (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

// AppleWin's initChromaPhaseTables, monitor variant
void BuildNtscTables(NtscTables& t) {
    const float kPi = 3.1415926535898f;
    const double kRad45 = kPi * 0.25f;
    const double kRad90 = kPi * 0.5f;
    const double kCycleStart = kPi * 45 / 180.f;
    Filter signal;
    Filter chroma;
    Filter luma0;
    Filter luma1;
    for (int phase = 0; phase < kPhases; phase++) {
        double phi = phase * kRad90 + kCycleStart;
        for (int s = 0; s < kSequences; s++) {
            int bits = s;
            double z = 0;
            double y0 = 0;
            double c = 0;
            double i = 0;
            double q = 0;
            for (int n = 0; n < 12; n++) {
                z = (bits & 0x800) != 0 ? 1.0 : 0.0;
                bits <<= 1;
                for (int k = 0; k < 2; k++) {
                    double zz = FilterSignal(signal, z);
                    c = FilterChroma(chroma, zz);
                    y0 = FilterLuma(luma0, zz);
                    FilterLuma(luma1, zz - c); // the tv variant's luma; kept so states match
                    c = c * 2.f;
                    i = i + (c * std::cos(phi) - i) / 8.f;
                    q = q + (c * std::sin(phi) - q) / 8.f;
                    phi += kRad45;
                }
            }
            float dot = ClampUnit(z);
            t.mono[s] = Rgb(dot, dot, dot);

            float r = ClampUnit(y0 + 0.956f * i + 0.621f * q);
            float g = ClampUnit(y0 + -0.272f * i + -0.647f * q);
            float b = ClampUnit(y0 + -1.105f * i + 1.702f * q);
            int color = s & 15;
            if (color == 15) {
                r = g = b = 1.f; // no ringing on white
            } else if (color == 0) {
                r = g = b = 0.f; // no ghosting on black
            } else if (color == 5) {
                r = g = b = static_cast<float>(0x83) / 255.f; // greys without chroma
            } else if (color == 10) {
                r = g = b = static_cast<float>(0x78) / 255.f;
            }
            t.hue[phase][s] = Rgb(r, g, b);
        }
    }
}

uint8_t RotateLeft4(int bits, int by) {
    by &= 3;
    return static_cast<uint8_t>(((bits << by) | (bits >> (4 - by))) & 15);
}

// a steady 4-dot pattern p, seen at phase k, has the last 4 dots RotateLeft4(p, k): each new
// dot is the one 4 dots back, so the window rotates left once per dot as the phase steps
void BuildFlatTable(NtscTables& t) {
    uint32_t steady[16];
    for (int p = 0; p < 16; p++) {
        uint32_t sum[3] = {};
        for (int k = 0; k < kPhases; k++) {
            int w = RotateLeft4(p, k);
            uint32_t rgb = t.hue[k][w | (w << 4) | (w << 8)];
            sum[0] += (rgb >> 16) & 0xFF;
            sum[1] += (rgb >> 8) & 0xFF;
            sum[2] += rgb & 0xFF;
        }
        steady[p] = 0xFF000000u | ((sum[0] / kPhases) << 16) | ((sum[1] / kPhases) << 8) |
                    (sum[2] / kPhases);
    }
    for (int k = 0; k < kPhases; k++) {
        for (int w = 0; w < 16; w++) {
            t.flat[k][w] = steady[RotateLeft4(w, 4 - k)];
        }
    }
    // a lo-res nibble's 4 dots start at phase 0 on even columns (odd ones are shifted to
    // match), lowest bit first, so after them the window is the nibble reversed at phase 3
    for (int n = 0; n < 16; n++) {
        int reversed = ((n & 1) << 3) | ((n & 2) << 1) | ((n & 4) >> 1) | ((n & 8) >> 3);
        t.lores[n] = t.flat[3][reversed];
    }
    // a lone hi-res pixel is 2 dots, at phase 0-1 on even columns and 2-3 on odd, one dot
    // later with bit 7 set; seen after its second dot the window is 0b0011
    for (int palette = 0; palette < 2; palette++) {
        for (int parity = 0; parity < 2; parity++) {
            t.hires[palette][parity] = t.flat[(palette + 2 * parity + 1) & 3][3];
        }
    }
}

const NtscTables& Tables() {
    static const NtscTables* tables = [] {
        static NtscTables built;
        BuildNtscTables(built);
        BuildFlatTable(built);
        return &built;
    }();
    return *tables;
}

// 7 dots to 14: each 280-wide dot covers two 14 MHz dots
std::array<uint16_t, 128> BuildDoubleTable() {
    std::array<uint16_t, 128> table = {};
    for (int byte = 0; byte < 128; byte++) {
        for (int bit = 0; bit < 7; bit++) {
            if ((byte & (1 << bit)) != 0) {
                table[byte] = static_cast<uint16_t>(table[byte] | (3 << (bit * 2)));
            }
        }
    }
    return table;
}

const std::array<uint16_t, 128> kDouble = BuildDoubleTable();

struct LineState {
    const NtscTables* tables = nullptr;
    uint32_t* row = nullptr;
    bool color = false;
    int out = 0;
    int phase = 0;
    uint16_t signal = 0;
    uint16_t last_dot = 0;
};

void PushDot(LineState& s, uint16_t dot) {
    s.signal = static_cast<uint16_t>(((s.signal << 1) | dot) & 0xFFF);
    uint32_t pixel = s.color ? s.tables->hue[s.phase][s.signal] : s.tables->mono[s.signal];
    if (s.out >= 0 && s.out < kScreenWidth) {
        s.row[s.out] = pixel;
    }
    s.out++;
    s.phase = (s.phase + 1) & 3;
}

void PushColumn(LineState& s, uint16_t bits) {
    for (int i = 0; i < 14; i++) {
        PushDot(s, (bits >> i) & 1);
    }
    s.last_dot = (bits >> 13) & 1;
}

// one character's 7 dots on a glyph row
uint8_t TextDots(uint8_t byte, bool altchar, int glyph_row, bool flash_inverse) {
    TextCell cell = DecodeTextByte(byte, altchar);
    uint8_t dots = CellRow(cell, glyph_row);
    bool inverse =
        cell.style == TextStyle::kInverse || (cell.style == TextStyle::kFlash && flash_inverse);
    if (inverse) {
        dots ^= 0x7F;
    }
    return static_cast<uint8_t>(dots & 0x7F);
}

uint16_t TextBits(uint8_t byte, bool altchar, int glyph_row, bool flash_inverse) {
    return kDouble[TextDots(byte, altchar, glyph_row, flash_inverse)];
}

// the low nibble rotated right one bit
uint8_t RotateRight4(int bits) {
    bits &= 15;
    return static_cast<uint8_t>(((bits >> 1) | (bits << 3)) & 15);
}

// a lo-res nibble's dots from absolute dot `start` on: dot p shows bit p mod 4
uint16_t LoresDots(uint8_t nibble, int start) {
    uint32_t pattern = static_cast<uint32_t>(nibble) * 0x11111;
    return static_cast<uint16_t>(pattern >> (start & 3));
}

// the 14 dots of a column in 80 columns or double-res: 7 from the aux byte, then 7 from main.
// double lo-res shows an aux nibble rotated right one bit
uint16_t DoubleBits(const VideoFrame& frame, int line, int col) {
    uint8_t main = frame.bytes[line][col];
    uint8_t aux = frame.aux_bytes[line][col];
    uint8_t mode = frame.modes[line][col];
    uint16_t first = 0;
    uint16_t second = 0;
    switch (mode & 0x03) {
    case kVideoText: {
        bool alt = (mode & kVideoAltChar) != 0;
        first = TextDots(aux, alt, line & 7, frame.flash_inverse);
        second = TextDots(main, alt, line & 7, frame.flash_inverse);
        break;
    }
    case kVideoLores: {
        int shift = line & 4;
        first = LoresDots(RotateRight4(aux >> shift), col * 14);
        second = LoresDots(static_cast<uint8_t>((main >> shift) & 15), col * 14 + 7);
        break;
    }
    default:
        first = aux;
        second = main;
        break;
    }
    return static_cast<uint16_t>((first & 0x7F) | ((second & 0x7F) << 7));
}

// the flat look of a column in a double mode: text and no-burst lines as plain dots, lo-res
// halves as blocks, double hi-res by 4-dot groups (the group's dots as a lo-res nibble)
void FlatDoubleColumn(const VideoFrame& frame, int line, int col, const uint16_t* hires_groups,
                      uint32_t* row) {
    const NtscTables& t = Tables();
    const uint32_t kBlack = 0xFF000000u;
    const uint32_t kWhite = 0xFFFFFFFFu;
    uint8_t mode = frame.modes[line][col];
    bool color = frame.burst[line] && (mode & 0x03) != kVideoText;
    uint16_t bits = DoubleBits(frame, line, col);
    uint32_t* out = row + col * 14;
    if (!color) {
        for (int i = 0; i < 14; i++) {
            out[i] = ((bits >> i) & 1) != 0 ? kWhite : kBlack;
        }
        return;
    }
    if ((mode & 0x03) == kVideoLores) {
        int shift = line & 4;
        uint32_t aux = t.lores[RotateRight4(frame.aux_bytes[line][col] >> shift)];
        uint32_t main = t.lores[(frame.bytes[line][col] >> shift) & 15];
        for (int i = 0; i < 14; i++) {
            out[i] = i < 7 ? aux : main;
        }
        return;
    }
    for (int i = 0; i < 14; i++) {
        int dot = col * 14 + i;
        out[i] = t.lores[hires_groups[dot / 4]];
    }
}

// the flat look: no signal, every dot set straight from the bytes
void RenderLineFlat(const VideoFrame& frame, int line, uint32_t* row) {
    const NtscTables& t = Tables();
    const uint32_t kBlack = 0xFF000000u;
    const uint32_t kWhite = 0xFFFFFFFFu;
    bool color = frame.burst[line];
    for (int i = 0; i < kScreenWidth; i++) {
        row[i] = kBlack;
    }
    // the line's 280 hi-res pixels and their palette bits, so neighbours can be seen
    // across bytes; columns in other modes read as off
    bool on[282] = {};
    uint8_t palette[282] = {};
    for (int col = 0; col < static_cast<int>(kVisibleColumns); col++) {
        if ((frame.modes[line][col] & (0x03 | kVideoDouble)) != kVideoHires) {
            continue;
        }
        uint8_t byte = frame.bytes[line][col];
        for (int bit = 0; bit < 7; bit++) {
            on[1 + col * 7 + bit] = (byte & (1 << bit)) != 0;
            palette[1 + col * 7 + bit] = static_cast<uint8_t>(byte >> 7);
        }
    }
    // double hi-res dots in 4-dot groups, as lo-res nibbles
    uint16_t groups[kScreenWidth / 4] = {};
    for (int col = 0; col < static_cast<int>(kVisibleColumns); col++) {
        uint8_t mode = frame.modes[line][col];
        if ((mode & kVideoDouble) == 0 || (mode & 0x03) != kVideoHires) {
            continue;
        }
        uint16_t bits = DoubleBits(frame, line, col);
        for (int i = 0; i < 14; i++) {
            int dot = col * 14 + i;
            groups[dot / 4] =
                static_cast<uint16_t>(groups[dot / 4] | (((bits >> i) & 1) << (dot & 3)));
        }
    }
    for (int col = 0; col < static_cast<int>(kVisibleColumns); col++) {
        uint8_t byte = frame.bytes[line][col];
        uint8_t mode = frame.modes[line][col];
        uint32_t* out = row + col * 14;
        if ((mode & kVideoDouble) != 0) {
            FlatDoubleColumn(frame, line, col, groups, row);
            continue;
        }
        if ((mode & 0x03) == kVideoText) {
            uint16_t bits =
                TextBits(byte, (mode & kVideoAltChar) != 0, line & 7, frame.flash_inverse);
            for (int i = 0; i < 14; i++) {
                out[i] = ((bits >> i) & 1) != 0 ? kWhite : kBlack;
            }
            continue;
        }
        if ((mode & 0x03) == kVideoLores) {
            uint8_t nibble = (byte >> (line & 4)) & 0x0F;
            uint32_t fill = color ? t.lores[nibble] : kBlack;
            if (!color) {
                // no burst: show the dot pattern in white, like a mono monitor
                uint16_t pattern = static_cast<uint16_t>((nibble * 0x1111) >> ((col & 1) * 2));
                for (int i = 0; i < 14; i++) {
                    out[i] = ((pattern >> i) & 1) != 0 ? kWhite : kBlack;
                }
                continue;
            }
            for (int i = 0; i < 14; i++) {
                out[i] = fill;
            }
            continue;
        }
        // hi-res: two on pixels side by side are white, a lone one takes its column's color,
        // and an off pixel between two on ones fills with their color; bit 7 shifts a dot
        for (int bit = 0; bit < 7; bit++) {
            int x = 1 + col * 7 + bit;
            int parity = (x - 1) & 1;
            uint32_t pixel = kBlack;
            if (on[x]) {
                bool white = !color || on[x - 1] || on[x + 1];
                pixel = white ? kWhite : t.hires[palette[x]][parity];
            } else if (color && on[x - 1] && on[x + 1]) {
                pixel = t.hires[palette[x - 1]][parity ^ 1];
            }
            int dot = (x - 1) * 2 + palette[x];
            for (int i = 0; i < 2; i++) {
                if (dot + i < kScreenWidth) {
                    row[dot + i] = pixel;
                }
            }
        }
    }
}

void RenderLine(const VideoFrame& frame, int line, VideoLook look, uint32_t* row) {
    if (look == VideoLook::kColor) {
        RenderLineFlat(frame, line, row);
        return;
    }
    LineState s;
    s.tables = &Tables();
    s.row = row;
    s.color = frame.burst[line] && look != VideoLook::kMono;
    // the color filter lags two dots; AppleWin starts colored lines two dots early
    s.out = s.color ? -2 : 0;
    for (int col = 0; col < static_cast<int>(kVisibleColumns); col++) {
        uint8_t byte = frame.bytes[line][col];
        uint8_t mode = frame.modes[line][col];
        uint16_t bits = 0;
        if ((mode & kVideoDouble) != 0) {
            PushColumn(s, DoubleBits(frame, line, col));
            continue;
        }
        switch (mode & 0x03) {
        case kVideoText:
            bits = TextBits(byte, (mode & kVideoAltChar) != 0, line & 7, frame.flash_inverse);
            break;
        case kVideoLores: {
            uint16_t nibble = (byte >> (line & 4)) & 0x0F;
            uint16_t pattern = static_cast<uint16_t>(nibble * 0x1111);
            // odd columns start two dots into the pattern, so a color stays one color
            bits = static_cast<uint16_t>(pattern >> ((col & 1) * 2));
            break;
        }
        default:
            bits = kDouble[byte & 0x7F];
            if ((byte & 0x80) != 0) {
                bits = static_cast<uint16_t>((bits << 1) | s.last_dot); // half-dot delay
            }
            break;
        }
        PushColumn(s, bits);
        if ((mode & 0x03) == kVideoHires && col == static_cast<int>(kVisibleColumns) - 1) {
            s.last_dot = 0;
        }
    }
    // the last dot, then one dot of ringing
    PushDot(s, s.last_dot);
    PushDot(s, 0);
}

} // namespace

const char* VideoLookName(VideoLook look) {
    switch (look) {
    case VideoLook::kColor:
        return "color";
    case VideoLook::kNtsc:
        return "ntsc";
    case VideoLook::kMono:
        return "mono";
    }
    return "color";
}

void RenderFrame(const VideoFrame& frame, VideoLook look, Framebuffer& fb) {
    fb.pixels.resize(kScreenWidth * kScreenHeight);
    for (int line = 0; line < kScreenHeight; line++) {
        RenderLine(frame, line, look, fb.pixels.data() + line * kScreenWidth);
    }
}

void RenderScreen(const Apple2& a, VideoLook look, Framebuffer& fb) {
    VideoFrame frame;
    CaptureScreen(a, frame);
    RenderFrame(frame, look, fb);
}

} // namespace tisple
