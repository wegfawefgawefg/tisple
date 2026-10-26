#include "machine/png.hpp"

#include <array>
#include <cstdio>

namespace tisple {

namespace {

std::array<uint32_t, 256> BuildCrcTable() {
    std::array<uint32_t, 256> table = {};
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[n] = c;
    }
    return table;
}

const std::array<uint32_t, 256> kCrcTable = BuildCrcTable();

uint32_t Crc(const uint8_t* bytes, size_t count) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < count; i++) {
        c = kCrcTable[(c ^ bytes[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

void PutBe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void PutChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    PutBe32(out, static_cast<uint32_t>(data.size()));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    PutBe32(out, Crc(out.data() + start, out.size() - start));
}

// zlib stream of stored deflate blocks
std::vector<uint8_t> ZlibStore(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out = {0x78, 0x01};
    size_t pos = 0;
    do {
        size_t len = raw.size() - pos;
        len = len > 65535 ? 65535 : len;
        bool last = pos + len == raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<uint8_t>(len));
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(~len));
        out.push_back(static_cast<uint8_t>(~len >> 8));
        out.insert(out.end(), raw.begin() + pos, raw.begin() + pos + len);
        pos += len;
    } while (pos < raw.size());
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    PutBe32(out, (b << 16) | a);
    return out;
}

} // namespace

std::vector<uint8_t> EncodePng(const uint32_t* pixels, int width, int height) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + width * 3));
    for (int y = 0; y < height; y++) {
        raw.push_back(0); // filter: none
        for (int x = 0; x < width; x++) {
            uint32_t p = pixels[y * width + x];
            raw.push_back(static_cast<uint8_t>(p >> 16));
            raw.push_back(static_cast<uint8_t>(p >> 8));
            raw.push_back(static_cast<uint8_t>(p));
        }
    }
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> header;
    PutBe32(header, static_cast<uint32_t>(width));
    PutBe32(header, static_cast<uint32_t>(height));
    header.push_back(8); // bit depth
    header.push_back(2); // rgb
    header.push_back(0);
    header.push_back(0);
    header.push_back(0);
    PutChunk(out, "IHDR", header);
    PutChunk(out, "IDAT", ZlibStore(raw));
    PutChunk(out, "IEND", {});
    return out;
}

bool WriteScreenPng(const std::string& path, const Framebuffer& fb, std::string& error) {
    std::vector<uint32_t> doubled(kScreenWidth * kScreenHeight * 2);
    for (int y = 0; y < kScreenHeight; y++) {
        for (int x = 0; x < kScreenWidth; x++) {
            uint32_t p = fb.pixels[y * kScreenWidth + x];
            doubled[(2 * y) * kScreenWidth + x] = p;
            doubled[(2 * y + 1) * kScreenWidth + x] = p;
        }
    }
    std::vector<uint8_t> png = EncodePng(doubled.data(), kScreenWidth, kScreenHeight * 2);
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        error = "cannot write '" + path + "'";
        return false;
    }
    bool ok = std::fwrite(png.data(), 1, png.size(), file) == png.size();
    ok = std::fclose(file) == 0 && ok;
    if (!ok) {
        error = "cannot write '" + path + "'";
    }
    return ok;
}

uint32_t FramebufferChecksum(const Framebuffer& fb) {
    uint32_t h = 2166136261u;
    for (uint32_t p : fb.pixels) {
        for (int i = 0; i < 3; i++) {
            h ^= (p >> (8 * i)) & 0xFF;
            h *= 16777619u;
        }
    }
    return h;
}

} // namespace tisple
