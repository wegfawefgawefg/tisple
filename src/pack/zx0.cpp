#include "pack/zx0.hpp"

#include <algorithm>
#include <utility>

namespace tisple {

namespace {

constexpr int kMaxOffset = 32640;  // the format's farthest match: 255 * 128
constexpr int kSmallOffsets = 128; // tried one by one: their high part costs a bit
constexpr int kChainDepth = 32;    // entries looked at in each hash chain
constexpr int kNice = 128;         // a match this long is taken as it is
constexpr int kEveryLength = 32;   // match lengths tried one by one, then gamma sizes' ends
constexpr int kSlots = 6;          // ways kept to reach each byte
constexpr int kLevels = 4;
constexpr int kLevelLength[kLevels] = {3, 8, 24, 64};

int GammaBits(int value) {
    int bits = 1;
    while (value >>= 1) {
        bits += 2;
    }
    return bits;
}

int OffsetBits(int offset) {
    return 8 + GammaBits((offset - 1) / 128 + 1); // the step's bit, the high part, the low 7
}

enum class Step : std::uint8_t {
    Start,
    Literal,  // one byte of a literal run
    Repeat,   // a match at the last offset, after literals only
    NewMatch, // a match at a new offset
};

// one way to reach a byte: what it cost in bits, the last offset, the step that got here
struct Arrival {
    int cost = 0;
    int from = -1;  // the byte it came from, and which of its slots
    int run = 0;    // literal run so far, for Literal
    int length = 0; // of this step
    std::uint16_t rep = 1;
    std::int8_t from_slot = -1;
    Step step = Step::Start;
};

struct Reach {
    Arrival slot[kSlots];
    int used = 0;
};

// keeps the cheapest way for each last offset, literal ways apart from match ways, and the
// kSlots cheapest of those
void Offer(Reach& r, const Arrival& a) {
    bool literal = a.step == Step::Literal;
    for (int i = 0; i < r.used; i++) {
        Arrival& b = r.slot[i];
        if (b.rep == a.rep && (b.step == Step::Literal) == literal) {
            if (a.cost < b.cost) {
                b = a;
            }
            return;
        }
    }
    if (r.used < kSlots) {
        r.slot[r.used++] = a;
        return;
    }
    int worst = 0;
    for (int i = 1; i < kSlots; i++) {
        if (r.slot[i].cost > r.slot[worst].cost) {
            worst = i;
        }
    }
    if (a.cost < r.slot[worst].cost) {
        r.slot[worst] = a;
    }
}

int Cheapest(const Reach& r) {
    int best = 0;
    for (int i = 1; i < r.used; i++) {
        if (r.slot[i].cost < r.slot[best].cost) {
            best = i;
        }
    }
    return best;
}

int MatchLength(const std::vector<std::uint8_t>& d, int at, int offset, int cap) {
    int limit = std::min(static_cast<int>(d.size()) - at, cap);
    int length = 0;
    while (length < limit && d[at + length] == d[at + length - offset]) {
        length++;
    }
    return length;
}

// the lengths worth trying from lo to hi: each up to kEveryLength, then the longest of each
// gamma size, then hi
void LengthsToTry(int lo, int hi, std::vector<int>& out) {
    out.clear();
    for (int m = lo; m <= hi && m <= kEveryLength; m++) {
        out.push_back(m);
    }
    for (int size = 64; size / 2 <= hi && size <= 65536; size *= 2) {
        int m = size - 1;
        if (m > kEveryLength && m >= lo && m < hi) {
            out.push_back(m);
        }
    }
    if (hi > kEveryLength && hi >= lo) {
        out.push_back(hi);
    }
}

struct Candidate {
    int offset = 0;
    int length = 0;
};

// matches at each byte, nearest offsets first, each longer than every nearer one
struct Finder {
    const std::vector<std::uint8_t>& d;
    std::vector<int> head[kLevels];
    std::vector<int> prev[kLevels];
    std::vector<std::uint32_t> hash[kLevels];
    int inserted = 0;
    int carry_offset = 0; // a long match found one byte back goes on a byte shorter
    int carry_length = 0;

    explicit Finder(const std::vector<std::uint8_t>& data) : d(data) {
        int n = static_cast<int>(d.size());
        for (int l = 0; l < kLevels; l++) {
            head[l].assign(65536, -1);
            prev[l].assign(n, -1);
            hash[l].assign(n, 0);
            int k = kLevelLength[l];
            std::uint32_t power = 1;
            for (int t = 0; t < k; t++) {
                power *= 0x9E3779B1u;
            }
            std::uint32_t h = 0;
            for (int t = 0; t < n; t++) {
                h = h * 0x9E3779B1u + d[t] + 1;
                if (t >= k) {
                    h -= power * (d[t - k] + 1u);
                }
                if (t + 1 >= k) {
                    hash[l][t + 1 - k] = h;
                }
            }
        }
    }

    void InsertUpTo(int at) {
        int n = static_cast<int>(d.size());
        for (; inserted < at; inserted++) {
            for (int l = 0; l < kLevels && inserted + kLevelLength[l] <= n; l++) {
                int key = static_cast<int>(hash[l][inserted] >> 16);
                prev[l][inserted] = head[l][key];
                head[l][key] = inserted;
            }
        }
    }

    void Find(int at, std::vector<Candidate>& out) {
        out.clear();
        if (carry_length > kNice) {
            carry_length--;
            out.push_back({carry_offset, carry_length});
            return;
        }
        carry_length = 0;
        InsertUpTo(at);
        int longest = 1;
        for (int offset = 1; offset <= kSmallOffsets && offset <= at; offset++) {
            int length = MatchLength(d, at, offset, kNice);
            if (length > longest) {
                longest = length;
                out.push_back({offset, length});
            }
        }
        std::size_t small = out.size();
        int n = static_cast<int>(d.size());
        for (int l = 0; l < kLevels && longest < kNice; l++) {
            if (at + kLevelLength[l] > n || (l > 0 && longest < kLevelLength[l - 1])) {
                break;
            }
            int j = head[l][hash[l][at] >> 16];
            int level_best = 0;
            for (int depth = 0; j >= 0 && depth < kChainDepth; depth++, j = prev[l][j]) {
                int offset = at - j;
                if (offset > kMaxOffset) {
                    break;
                }
                if (offset <= kSmallOffsets) {
                    continue;
                }
                int length = MatchLength(d, at, offset, kNice);
                if (length > level_best && length >= kLevelLength[l]) {
                    level_best = length;
                    out.push_back({offset, length});
                    longest = std::max(longest, length);
                    if (length >= kNice) {
                        break;
                    }
                }
            }
        }
        KeepFront(small, out);
        if (!out.empty() && out.back().length >= kNice) {
            out.back().length = MatchLength(d, at, out.back().offset, n);
            carry_offset = out.back().offset;
            carry_length = out.back().length;
        }
    }

    // by offset, each longer than all nearer: a farther offset never costs less
    static void KeepFront(std::size_t sorted_upto, std::vector<Candidate>& out) {
        if (sorted_upto == out.size()) {
            return;
        }
        std::sort(out.begin(), out.end(),
                  [](const Candidate& a, const Candidate& b) { return a.offset < b.offset; });
        std::size_t kept = 0;
        int longest = 1;
        for (const Candidate& c : out) {
            if (c.length > longest) {
                longest = c.length;
                out[kept++] = c;
            }
        }
        out.resize(kept);
    }
};

struct BitWriter {
    std::vector<std::uint8_t> out;
    std::size_t bit_byte = 0;
    int bit_mask = 0;
    bool backtrack = true; // the next bit goes in the last byte's bit 0

    void Byte(int value) {
        out.push_back(static_cast<std::uint8_t>(value));
    }

    void Bit(bool value) {
        if (backtrack) {
            if (value) {
                out.back() |= 1;
            }
            backtrack = false;
            return;
        }
        if (bit_mask == 0) {
            bit_mask = 128;
            bit_byte = out.size();
            Byte(0);
        }
        if (value) {
            out[bit_byte] |= static_cast<std::uint8_t>(bit_mask);
        }
        bit_mask >>= 1;
    }

    // interlaced: a 0 and the next bit down for each bit below the top one, then a 1
    void Gamma(int value, bool invert) {
        int top = 1;
        while (top * 2 <= value) {
            top *= 2;
        }
        for (int bit = top / 2; bit > 0; bit /= 2) {
            Bit(false);
            bool b = (value & bit) != 0;
            Bit(invert ? !b : b);
        }
        Bit(true);
    }
};

// the steps from the cheapest arrival at the end back to the start, literal bytes as runs
struct Token {
    Step step = Step::Literal;
    int length = 0;
    int offset = 0;
};

std::vector<Token> Path(const std::vector<Reach>& reach, int n) {
    std::vector<Token> back;
    int at = n;
    int slot = Cheapest(reach[n]);
    while (at > 0) {
        const Arrival& a = reach[at].slot[slot];
        if (a.step == Step::Literal && !back.empty() && back.back().step == Step::Literal) {
            back.back().length++;
        } else {
            back.push_back({a.step, a.length, a.rep});
        }
        at = a.from;
        slot = a.from_slot;
    }
    std::reverse(back.begin(), back.end());
    return back;
}

std::vector<std::uint8_t> Write(const std::vector<std::uint8_t>& d,
                                const std::vector<Token>& tokens) {
    BitWriter w;
    std::size_t in = 0;
    for (const Token& t : tokens) {
        if (t.step == Step::Literal) {
            w.Bit(false);
            w.Gamma(t.length, false);
            for (int i = 0; i < t.length; i++) {
                w.Byte(d[in++]);
            }
        } else if (t.step == Step::Repeat) {
            w.Bit(false);
            w.Gamma(t.length, false);
            in += static_cast<std::size_t>(t.length);
        } else {
            w.Bit(true);
            w.Gamma((t.offset - 1) / 128 + 1, true);
            w.Byte((127 - (t.offset - 1) % 128) << 1);
            w.backtrack = true; // the length's first bit is the low byte's bit 0
            w.Gamma(t.length - 1, false);
            in += static_cast<std::size_t>(t.length);
        }
    }
    w.Bit(true); // the end: a new offset whose high part is 256
    w.Gamma(256, true);
    return w.out;
}

void StepLiteral(std::vector<Reach>& reach, int at, int slot) {
    const Arrival& a = reach[at].slot[slot];
    Arrival x;
    x.step = Step::Literal;
    x.rep = a.rep;
    x.from = at;
    x.from_slot = static_cast<std::int8_t>(slot);
    x.length = 1;
    if (a.step == Step::Literal) {
        x.run = a.run + 1;
        x.cost = a.cost + 8 + GammaBits(x.run) - GammaBits(a.run);
    } else {
        x.run = 1;
        x.cost = a.cost + 1 + GammaBits(1) + 8;
    }
    Offer(reach[at + 1], x);
}

void StepRepeat(const std::vector<std::uint8_t>& d, std::vector<Reach>& reach, int at, int slot,
                std::vector<int>& lengths) {
    const Arrival& a = reach[at].slot[slot];
    if (a.step != Step::Literal || a.rep > at) {
        return;
    }
    int most = MatchLength(d, at, a.rep, static_cast<int>(d.size()));
    if (most == 0) {
        return;
    }
    LengthsToTry(1, most, lengths);
    for (int m : lengths) {
        Arrival y;
        y.step = Step::Repeat;
        y.rep = a.rep;
        y.cost = a.cost + 1 + GammaBits(m);
        y.from = at;
        y.from_slot = static_cast<std::int8_t>(slot);
        y.length = m;
        Offer(reach[at + m], y);
    }
}

// a new offset costs the same from every way here, so only the cheapest goes on
void StepNewMatches(std::vector<Reach>& reach, int at, const std::vector<Candidate>& found,
                    std::vector<int>& lengths) {
    int best = Cheapest(reach[at]);
    const Arrival& b = reach[at].slot[best];
    int lo = 2;
    for (const Candidate& c : found) {
        LengthsToTry(lo, c.length, lengths);
        int offset_bits = OffsetBits(c.offset);
        for (int m : lengths) {
            Arrival y;
            y.step = Step::NewMatch;
            y.rep = static_cast<std::uint16_t>(c.offset);
            y.cost = b.cost + offset_bits + GammaBits(m - 1);
            y.from = at;
            y.from_slot = static_cast<std::int8_t>(best);
            y.length = m;
            Offer(reach[at + m], y);
        }
        lo = c.length + 1;
    }
}

} // namespace

std::vector<std::uint8_t> PackZx0(const std::vector<std::uint8_t>& data) {
    int n = static_cast<int>(data.size());
    if (n == 0) {
        return {};
    }
    std::vector<Reach> reach(n + 1);
    reach[0].slot[0].cost = -1; // the first step is a literal run, with no bit before it
    reach[0].used = 1;
    Finder finder(data);
    std::vector<Candidate> found;
    std::vector<int> lengths;
    for (int at = 0; at < n; at++) {
        if (reach[at].used == 0) {
            continue;
        }
        for (int s = 0; s < reach[at].used; s++) {
            StepLiteral(reach, at, s);
            StepRepeat(data, reach, at, s, lengths);
        }
        if (at == 0 || at + 1 >= n) {
            continue;
        }
        finder.Find(at, found);
        StepNewMatches(reach, at, found, lengths);
    }
    return Write(data, Path(reach, n));
}

} // namespace tisple
