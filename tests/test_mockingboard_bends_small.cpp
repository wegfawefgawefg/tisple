// lib/mockingboard_bends_small against lib/mockingboard_bends on the machine with the emulated
// card: songs with w (vibrato with a delay), p (glide) and t (detune), and an effect with them,
// played by service_bends and by service_bends_small give the same registers tick for tick; an
// s plays as no bend; the small player is smaller.

#include "machine/mockingboard.hpp"
#include "test_mockingboard.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

// a_waltz_vib's way of using them (Catwiz's rest): a tune doubled a unit apart, a late vibrato
// on its long notes, a glide up into one; and every depth, speed and glide speed, detunes both
// ways, glides both ways and to the ends of the notes, a w0 and a p0
const char* kSong = R"(
(import mockingboard mb)
(import mockingboard_song tune)
(tune.song bent
    (pattern in_tune "l1 v10 k5 w2:12:14 r")
    (pattern in_double "l1 v10 k5 w2:12:14 t+1 r")
    (pattern tune_a "l48 a5 l24 f5 l12 e5 f5 l24 e5 d5 l48 a#5 l24 g5 l48 a5 l24 - p5 l48 d6 p0 l24 a5 a#5")
    (pattern oom "l36 v12 k2 d2 r g2 r")
    (pattern depths "l20 v12 w1:2 c4 w3:64:3 e4 w4:10 g2 p1 c5 p7 c2 w0 p0 t-64 c6 t+64 c6 t0 c6")
    (pattern glides "l9 v11 p3 c3 g4 c3 p6 c8 c1 p2 a3 - r e3 p0 l30 v9 w2:7:1 c7")
    (voice 0 oom)
    (voice 1 depths)
    (voice 2 in_double loop tune_a)
    (voice 3 in_tune loop tune_a)
    (voice 4 glides))
(fn irq () void :interrupt (mb.service_bends))
(fn main () void
    (mb.init)
    (mb.play (addr_of (at bent 0)))
    (for i u16 0 760
        (wait_cycles 17030))
    (mb.off))
)";

// an effect with them, twice, on voice 1 (FirstTick's): the second glides from where the
// first left the slot
const char* kEffect = R"(
(import mockingboard mb)
(import mockingboard_song tune)
(tune.effect bell "l2 v15 k1 w3:6 c6 p4 g6 l20 e6 t-3 w1:4:2 c5")
(fn irq () void :interrupt (mb.service_bends))
(fn main () void
    (mb.init)
    (mb.effect (addr_of (at bell 0)) 1 1)
    (mb.effect (addr_of (at bell 0)) 1 1)
    (for i u8 0 100
        (wait_cycles 17030))
    (mb.off))
)";

constexpr int kTicks = 720;

std::string WithService(const char* text, const char* service) {
    std::string s = text;
    std::string bends = "(mb.service_bends)";
    s.replace(s.find(bends), bends.size(), service);
    return s;
}

bool Played(TestResults& results, const std::string& text, Voices& v, size_t& bytes,
            int ticks = kTicks) {
    MbBuild b;
    MbBuildText(results, "t.tsp", text, b);
    if (!b.ok) {
        return false;
    }
    bytes = b.code.image.size();
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    tisple::RunApple2(*a, (static_cast<uint64_t>(ticks) + 40) * kMbTick);
    v = ReadVoices(*a, FirstTick(*a), ticks);
    return true;
}

// the first tick where the two differ, or -1
int FirstDifference(const Voices& a, const Voices& b, int ticks = kTicks) {
    for (int t = 0; t < ticks; t++) {
        for (int k = 0; k < 6; k++) {
            if (a.period[k][t] != b.period[k][t] || a.volume[k][t] != b.volume[k][t]) {
                return t;
            }
        }
        if (a.mixer[0][t] != b.mixer[0][t] || a.mixer[1][t] != b.mixer[1][t]) {
            return t;
        }
    }
    return -1;
}

void SameTests(TestResults& results) {
    Voices bends;
    Voices small;
    size_t bends_bytes = 0;
    size_t small_bytes = 0;
    bool built =
        Played(results, kSong, bends, bends_bytes) &&
        Played(results, WithService(kSong, "(mb.service_bends_small)"), small, small_bytes);
    CHECK(results, built);
    if (!built) {
        return;
    }
    // the song does move: the doubled tune a unit apart, its vibrato away from the note
    CHECK(results, bends.period[2][30] == bends.period[3][30] + 1);
    bool wobbled = false;
    for (int t = 15; t < 48; t++) {
        wobbled = wobbled || bends.period[3][t] != bends.period[3][0];
    }
    CHECK(results, wobbled);
    int at = FirstDifference(bends, small);
    CHECK(results, at < 0);
    if (at >= 0) {
        std::fprintf(stderr, "bends_small: differs from bends at tick %d\n", at);
        for (int t = at - 2; t < at + 6 && t < kTicks; t++) {
            for (int k = 0; k < 6; k++) {
                std::fprintf(stderr, " %d:%d/%d-%d/%d", k, bends.period[k][t], bends.volume[k][t],
                             small.period[k][t], small.volume[k][t]);
            }
            std::fprintf(stderr, "\n");
        }
    }
    CHECK(results, small_bytes < bends_bytes);
    Voices fx_bends;
    Voices fx_small;
    built = Played(results, kEffect, fx_bends, bends_bytes, 80) &&
            Played(results, WithService(kEffect, "(mb.service_bends_small)"), fx_small, small_bytes,
                   80);
    CHECK(results, built && FirstDifference(fx_bends, fx_small, 80) < 0);
    std::printf("mockingboard: the bends' test song with service_bends %zu bytes, with "
                "service_bends_small %zu\n",
                bends_bytes, small_bytes);
}

// a slide is read and not played: the note holds its pitch, and what follows plays on
void SlideTests(TestResults& results) {
    const char* text = R"(
(import mockingboard mb)
(import mockingboard_song tune)
(tune.song slid
    (pattern a "l20 v12 s-3 c3 s+4 c5 s0 t+2 e4 l60 r")
    (voice 1 a))
(fn irq () void :interrupt (mb.service_bends_small))
(fn main () void
    (mb.init)
    (mb.play (addr_of (at slid 0)))
    (for i u8 0 80
        (wait_cycles 17030))
    (mb.off))
)";
    MbBuild b;
    MbBuildText(results, "t.tsp", text, b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    tisple::RunApple2(*a, 80 * kMbTick);
    Voices v = ReadVoices(*a, FirstTick(*a), 60);
    int c3 = Period(Note('c', false, 3));
    int c5 = Period(Note('c', false, 5));
    int e4 = Period(Note('e', false, 4));
    bool held = true;
    for (int t = 1; t < 19; t++) {
        held = held && v.period[1][t] == c3 && v.period[1][20 + t] == c5;
        held = held && v.period[1][40 + t] == e4 + 2;
    }
    CHECK(results, held);
    if (!held) {
        for (int t = 0; t < 60; t += 3) {
            std::fprintf(stderr, " %d", v.period[1][t]);
        }
        std::fprintf(stderr, " (c3 %d c5 %d e4 %d)\n", c3, c5, e4);
    }
}

} // namespace

void RunMockingboardBendsSmallTests(TestResults& results) {
    SameTests(results);
    SlideTests(results);
}

} // namespace tisple_tests
