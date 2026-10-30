// packed data: ZX0 packing round trips over random and made-up real data (pack/zx0.hpp), the
// decoder refusing broken streams, (pack bytes) in macro code and lib/tables' packed tables

#include "test_pack.hpp"

#include "check/expand.hpp"
#include "pack/zx0.hpp"
#include "reader/format.hpp"
#include "test_check.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace tisple_tests {

// ---- made-up double hi-res pictures

namespace {

constexpr int kWide = 140; // a pixel is 4 dots, its colour the dots as a lo-res nibble
constexpr int kTall = 192;

using Canvas = std::vector<uint8_t>; // kWide * kTall colours 0-15

void Rect(Canvas& c, int x0, int y0, int x1, int y1, uint8_t colour) {
    for (int y = std::max(0, y0); y < std::min(kTall, y1); y++) {
        for (int x = std::max(0, x0); x < std::min(kWide, x1); x++) {
            c[y * kWide + x] = colour;
        }
    }
}

// an ellipse with a one-pixel outline, rx in pixels (wide ones) and ry in rows
void Blob(Canvas& c, int cx, int cy, int rx, int ry, uint8_t fill, uint8_t edge) {
    for (int y = cy - ry - 1; y <= cy + ry + 1; y++) {
        for (int x = cx - rx - 1; x <= cx + rx + 1; x++) {
            if (x < 0 || y < 0 || x >= kWide || y >= kTall) {
                continue;
            }
            double dx = (x - cx) / static_cast<double>(rx);
            double dy = (y - cy) / static_cast<double>(ry);
            double d = dx * dx + dy * dy;
            if (d <= 1.0) {
                c[y * kWide + x] = d > 0.8 ? edge : fill;
            }
        }
    }
}

// row y's address in a hi-res page, from $0000
int RowOffset(int y) {
    return (y & 7) * 0x400 + ((y >> 3) & 7) * 0x80 + (y >> 6) * 0x28;
}

std::vector<uint8_t> Half(const Canvas& c, int half) {
    std::vector<uint8_t> page(8192, 0);
    for (int y = 0; y < kTall; y++) {
        for (int col = 0; col < 40; col++) {
            uint8_t b = 0;
            for (int i = 0; i < 7; i++) {
                int dot = col * 14 + i + (half == 0 ? 7 : 0); // aux's 7 dots come first
                uint8_t colour = c[y * kWide + dot / 4];
                b = static_cast<uint8_t>(b | (((colour >> (dot & 3)) & 1) << i));
            }
            page[RowOffset(y) + col] = b;
        }
    }
    return page;
}

} // namespace

std::vector<uint8_t> DhgrSample(uint32_t seed, int half) {
    TestRandom r{seed * 2654435761u + 7};
    Canvas c(kWide * kTall, 0);
    // sky bands with a dithered seam between each pair
    int y = 0;
    while (y < 120) {
        int h = 6 + r.Below(20);
        uint8_t a = static_cast<uint8_t>(r.Below(16));
        uint8_t b = static_cast<uint8_t>(r.Below(16));
        Rect(c, 0, y, kWide, y + h, a);
        for (int x = 0; x < kWide; x += 2) {
            c[std::min(kTall - 1, y + h - 1) * kWide + x + (y & 1)] = b;
        }
        y += h;
    }
    Rect(c, 0, 120, kWide, 160, static_cast<uint8_t>(r.Below(16))); // ground
    Rect(c, 0, 160, kWide, kTall, 0);                               // under the text window
    for (int k = 0; k < 6; k++) {
        Blob(c, r.Below(kWide), 40 + r.Below(110), 4 + r.Below(14), 6 + r.Below(24),
             static_cast<uint8_t>(r.Below(16)), 0);
    }
    for (int k = 0; k < 4; k++) {
        int x0 = r.Below(kWide);
        int y0 = r.Below(150);
        Rect(c, x0, y0, x0 + 3 + r.Below(20), y0 + 4 + r.Below(20),
             static_cast<uint8_t>(r.Below(16)));
    }
    for (int k = 0; k < 120; k++) { // stars, grass and dust
        c[r.Below(kTall) * kWide + r.Below(kWide)] = static_cast<uint8_t>(r.Below(16));
    }
    return Half(c, half);
}

std::vector<uint8_t> PackSample(TestRandom& r, int size) {
    std::vector<uint8_t> d;
    int shape = r.Below(6);
    while (static_cast<int>(d.size()) < size) {
        int part = shape == 5 ? r.Below(5) : shape; // 5: a mix of the others
        int n = 1 + r.Below(std::max(1, std::min(size, 400)));
        if (part == 0) { // noise
            for (int i = 0; i < n; i++) {
                d.push_back(static_cast<uint8_t>(r.Next()));
            }
        } else if (part == 1) { // few values
            int values = 2 + r.Below(3);
            for (int i = 0; i < n; i++) {
                d.push_back(static_cast<uint8_t>(r.Below(values) * 37));
            }
        } else if (part == 2) { // runs
            d.insert(d.end(), static_cast<size_t>(n), static_cast<uint8_t>(r.Below(4) * 85));
        } else if (d.size() > 0) { // a repeat, near or far
            int back = 1 + r.Below(static_cast<int>(d.size()));
            if (part == 4) {
                back = 1 + r.Below(std::min(8, static_cast<int>(d.size())));
            }
            size_t from = d.size() - static_cast<size_t>(back);
            for (int i = 0; i < n; i++) {
                d.push_back(d[from + static_cast<size_t>(i)]);
            }
        } else {
            d.push_back(static_cast<uint8_t>(r.Next()));
        }
    }
    d.resize(static_cast<size_t>(size));
    return d;
}

namespace {

bool RoundTrips(const std::vector<uint8_t>& data, size_t* packed_size = nullptr) {
    std::vector<uint8_t> packed = tisple::PackZx0(data);
    std::vector<uint8_t> out;
    bool ok = tisple::UnpackZx0(packed, data.size(), out) && out == data;
    if (packed_size != nullptr) {
        *packed_size = packed.size();
    }
    return ok;
}

std::vector<uint8_t> FileBytes(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>());
}

void EdgeTests(TestResults& results) {
    CHECK(results, tisple::PackZx0({}).empty());
    for (int n : {1, 2, 3, 4, 255, 256, 257, 4096}) {
        std::vector<uint8_t> zeros(static_cast<size_t>(n), 0);
        size_t size = 0;
        CHECK(results, RoundTrips(zeros, &size));
        CHECK(results, size <= 8);
        std::vector<uint8_t> count(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) {
            count[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
        }
        CHECK(results, RoundTrips(count));
    }
    // a match exactly the farthest offset back, and one byte farther (a literal run then)
    for (int gap : {32640, 32641}) {
        TestRandom r{99};
        std::vector<uint8_t> d(static_cast<size_t>(gap) + 300);
        for (uint8_t& b : d) {
            b = static_cast<uint8_t>(r.Next());
        }
        for (int i = 0; i < 300; i++) {
            d[static_cast<size_t>(gap + i)] = d[static_cast<size_t>(i)];
        }
        CHECK(results, RoundTrips(d));
    }
    std::vector<uint8_t> big(65535, 0x55);
    CHECK(results, RoundTrips(big));
}

// broken streams fail and never read or write past the ends
void BrokenTests(TestResults& results) {
    std::vector<uint8_t> data = DhgrSample(3, 0);
    std::vector<uint8_t> packed = tisple::PackZx0(data);
    std::vector<uint8_t> out;
    CHECK(results, tisple::UnpackZx0(packed, data.size(), out) && out == data);
    CHECK(results, !tisple::UnpackZx0(packed, data.size() - 1, out)); // no room for the last
    bool all_failed = true;
    for (size_t cut = 0; cut < packed.size(); cut += 7) {
        std::vector<uint8_t> part(packed.begin(), packed.begin() + static_cast<long>(cut));
        all_failed = all_failed && !tisple::UnpackZx0(part, 65536, out);
    }
    CHECK(results, all_failed);
    std::vector<uint8_t> longer = packed;
    longer.push_back(0);
    CHECK(results, !tisple::UnpackZx0(longer, 65536, out));
    // flipped bits: whatever comes out, it stops inside the limits
    TestRandom r{5};
    for (int k = 0; k < 300; k++) {
        std::vector<uint8_t> bent = packed;
        bent[static_cast<size_t>(r.Below(static_cast<int>(bent.size())))] ^=
            static_cast<uint8_t>(1 << r.Below(8));
        tisple::UnpackZx0(bent, 20000, out);
        CHECK(results, out.size() <= 20000);
    }
}

void FuzzTests(TestResults& results) {
    TestRandom r{20261030};
    int failed = 0;
    for (int k = 0; k < 400; k++) {
        int size = 1 + r.Below(k < 300 ? 600 : 9000);
        std::vector<uint8_t> d = PackSample(r, size);
        if (!RoundTrips(d)) {
            failed++;
            std::fprintf(stderr, "pack: sample %d (%d bytes) does not round trip\n", k, size);
        }
    }
    CHECK(results, failed == 0);
}

// real files: our own sources and docs (text), a compressed picture (noise to a packer),
// and made-up double hi-res pictures; the pictures must pack well
void RealDataTests(TestResults& results) {
    const char* files[] = {"lib/auxmem.tsp", "lib/mockingboard.tsp", "docs/language.md",
                           "src/pack/zx0.cpp", "docs/images/plasma.png"};
    for (const char* path : files) {
        std::vector<uint8_t> d = FileBytes(path);
        CHECK(results, !d.empty());
        if (d.size() > 65535) {
            d.resize(65535);
        }
        CHECK(results, d.empty() || RoundTrips(d));
    }
    size_t in = 0;
    size_t out = 0;
    for (uint32_t seed = 0; seed < 6; seed++) {
        for (int half = 0; half < 2; half++) {
            std::vector<uint8_t> d = DhgrSample(seed, half);
            size_t size = 0;
            CHECK(results, RoundTrips(d, &size));
            in += d.size();
            out += size;
        }
    }
    CHECK(results, out * 4 < in); // these pack to about a sixth
    std::printf("pack: made-up double hi-res pictures %zu -> %zu bytes (%.3f)\n", in, out,
                static_cast<double>(out) / static_cast<double>(in));
}

// ---- macro code and lib/tables

std::string Expanded(const std::string& text) {
    tisple::LoadOptions options;
    options.files.push_back(tisple::VirtualFile{"t.tsp", text});
    tisple::LoadedProgram loaded;
    std::vector<tisple::Diagnostic> errors;
    if (!tisple::LoadProgram("t.tsp", options, loaded, errors) ||
        !tisple::ExpandProgram(loaded, errors)) {
        return "error: " + tisple::FormatDiagnostics(loaded.sources, errors);
    }
    const tisple::Module& module = loaded.modules[0];
    return tisple::FormatTree(loaded.sources.files[module.file], module.tree, false);
}

void ExpectExpands(TestResults& results, const std::string& text, const std::string& want) {
    std::string got = Expanded(text);
    if (got.find(want) == std::string::npos) {
        std::fprintf(stderr, "pack macro: %s\n  want: %s\n  got:  %s", text.c_str(), want.c_str(),
                     got.c_str());
    }
    CHECK(results, got.find(want) != std::string::npos);
}

std::string ByteList(const std::vector<uint8_t>& bytes) {
    std::string s;
    for (uint8_t b : bytes) {
        s += (s.empty() ? "" : " ") + std::to_string(b);
    }
    return s;
}

void MacroTests(TestResults& results) {
    ExpectExpands(results,
                  "(macro v () `(make (array u8 3) ,@(pack (list 7))))\n"
                  "(const X (array u8 3) (v))",
                  "(make (array u8 3) " + ByteList(tisple::PackZx0({7})) + "))");
    // three bytes, three again from three back: a run, a match, the end
    std::vector<uint8_t> packed = tisple::PackZx0({1, 2, 3, 1, 2, 3, 9});
    std::string list = ByteList(packed);
    ExpectExpands(results, "(import tables)\n(tables.packed_bytes P (1 2 3 1 2 3 9))",
                  "(const P (array u8 " + std::to_string(packed.size()) + ") (make (array u8 " +
                      std::to_string(packed.size()) + ") " + list + "))");
    ExpectExpands(results,
                  "(import tables)\n(macro twice (i) (* i 2))\n"
                  "(tables.packed p 7 twice :in aux)",
                  "(let p (array u8 ");
    ExpectExpands(results,
                  "(import tables)\n(macro twice (i) (* i 2))\n"
                  "(tables.packed p 7 twice :in aux)",
                  ":in aux)");
    ExpectExpands(results, "(macro v () (pack (list 1 300)))\n(const X u8 (v))",
                  "pack takes bytes, 0 to 255; item 1 is 300");
    ExpectExpands(results, "(macro v () (pack (list)))\n(const X u8 (v))",
                  "pack takes 1 to 65535 bytes, got 0");
    ExpectExpands(results, "(macro v () (pack 5))\n(const X u8 (v))",
                  "pack needs a list of bytes, got");
}

} // namespace

void RunPackTests(TestResults& results) {
    EdgeTests(results);
    BrokenTests(results);
    FuzzTests(results);
    RealDataTests(results);
    MacroTests(results);
}

} // namespace tisple_tests
