// scanner fetches to pixels, through an ntsc model.
//
// ported from AppleWin's NTSC.cpp (GPL-2.0-or-later, see third_party/applewin; by William S
// Simms, Michael Pohoreski and Tom Charlesworth): every mode becomes a stream of 14 MHz dots,
// 560 per line. a 12-dot shift register of the latest dots, with the color phase (dot index
// mod 4), indexes a table built by filtering each 12-dot sequence and decoding it as yiq.
// that gives hi-res artifact color, the bit 7 half-dot shift, lo-res colors and color
// fringes on text in mixed mode, as AppleWin's "color monitor ntsc" shows them. lines
// without color burst (all-text screens) and the monochrome option use the plain dots.
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

const NtscTables& Tables() {
    static const NtscTables* tables = [] {
        static NtscTables built;
        BuildNtscTables(built);
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

uint16_t TextBits(uint8_t byte, bool altchar, int glyph_row, bool flash_inverse) {
    TextCell cell = DecodeTextByte(byte, altchar);
    uint8_t dots = GlyphRow(cell.ascii, glyph_row);
    bool inverse =
        cell.style == TextStyle::kInverse || (cell.style == TextStyle::kFlash && flash_inverse);
    if (inverse) {
        dots ^= 0x7F;
    }
    return kDouble[dots & 0x7F];
}

void RenderLine(const VideoFrame& frame, int line, bool mono, uint32_t* row) {
    LineState s;
    s.tables = &Tables();
    s.row = row;
    s.color = frame.burst[line] && !mono;
    // the color filter lags two dots; AppleWin starts colored lines two dots early
    s.out = s.color ? -2 : 0;
    for (int col = 0; col < static_cast<int>(kVisibleColumns); col++) {
        uint8_t byte = frame.bytes[line][col];
        uint8_t mode = frame.modes[line][col];
        uint16_t bits = 0;
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

void RenderFrame(const VideoFrame& frame, bool mono, Framebuffer& fb) {
    fb.pixels.resize(kScreenWidth * kScreenHeight);
    for (int line = 0; line < kScreenHeight; line++) {
        RenderLine(frame, line, mono, fb.pixels.data() + line * kScreenWidth);
    }
}

void RenderScreen(const Apple2& a, bool mono, Framebuffer& fb) {
    VideoFrame frame;
    CaptureScreen(a, frame);
    RenderFrame(frame, mono, fb);
}

} // namespace tisple
