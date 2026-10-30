// the plain ZX0 decoder (zx0.hpp): for tests and checks, never reading or writing outside

#include "pack/zx0.hpp"

namespace tisple {

namespace {

struct BitReader {
    const std::vector<std::uint8_t>& in;
    std::size_t at = 0;
    int mask = 0;
    int value = 0;
    int last = 0;
    bool backtrack = false;
    bool bad = false;

    int Byte() {
        if (at >= in.size()) {
            bad = true;
            return 0;
        }
        last = in[at++];
        return last;
    }

    int Bit() {
        if (backtrack) {
            backtrack = false;
            return last & 1;
        }
        mask >>= 1;
        if (mask == 0) {
            mask = 128;
            value = Byte();
        }
        return (value & mask) != 0 ? 1 : 0;
    }

    int Gamma(int invert) {
        int v = 1;
        while (!bad && Bit() == 0) {
            v = v << 1 | (Bit() ^ invert);
            if (v > 0x10000) {
                bad = true;
            }
        }
        return v;
    }
};

bool CopyMatch(std::vector<std::uint8_t>& out, int offset, int length, std::size_t max_out) {
    if (offset > static_cast<int>(out.size()) ||
        out.size() + static_cast<std::size_t>(length) > max_out) {
        return false;
    }
    for (int i = 0; i < length; i++) {
        out.push_back(out[out.size() - offset]);
    }
    return true;
}

} // namespace

bool UnpackZx0(const std::vector<std::uint8_t>& packed, std::size_t max_out,
               std::vector<std::uint8_t>& out) {
    out.clear();
    BitReader r{packed};
    int last = 1;
    bool literals = true;
    bool new_offset = false;
    while (!r.bad) {
        if (literals) {
            std::size_t length = static_cast<std::size_t>(r.Gamma(0));
            if (out.size() + length > max_out) {
                return false;
            }
            for (std::size_t i = 0; i < length; i++) {
                out.push_back(static_cast<std::uint8_t>(r.Byte()));
            }
            new_offset = r.Bit() == 1;
            literals = false;
            continue;
        }
        if (!new_offset) {
            if (!CopyMatch(out, last, r.Gamma(0), max_out)) {
                return false;
            }
            new_offset = r.Bit() == 1;
            literals = !new_offset;
            continue;
        }
        int high = r.Gamma(1);
        if (high == 256) {
            return !r.bad && r.at == packed.size();
        }
        if (high > 256) {
            return false;
        }
        last = high * 128 - (r.Byte() >> 1);
        r.backtrack = true;
        if (!CopyMatch(out, last, r.Gamma(0) + 1, max_out)) {
            return false;
        }
        new_offset = r.Bit() == 1;
        literals = !new_offset;
    }
    return false;
}

} // namespace tisple
