// lib/mockingboard's fade: (mb.fade 15) takes the music's voices down a step every 4 ticks,
// leaves the effect playing at its own volume and a voice on the hardware envelope as it is,
// and stops the music once it is silent; (mb.fade_in 13) brings the next song in from 13 steps
// down, a step every 4 ticks.

#include "machine/mockingboard.hpp"
#include "test_mockingboard.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

const char* kFade = R"(
(import mockingboard mb)
(import mockingboard_player player)
(import mockingboard_song tune)
(tune.song hum
    (pattern a "l64 v12 c3")
    (pattern e "h14:300 l64 c4")
    (pattern b "l64 v8 e3")
    (voice 0 a)
    (voice 1 e)
    (voice 3 b))
(tune.song next (pattern a "l64 v12 g3") (voice 0 a))
(tune.effect zap "l64 v15 c6")
(let waited u8 0 :address 0x03C0) ; outside zero page, which the end puts back
(let still u8 0 :address 0x03C1)
(fn irq () void :interrupt (mb.service))
(fn main () void
    (mb.init)
    (mb.play (addr_of (at hum 0)))
    (for i u8 0 10
        (wait_cycles 17030))
    (mb.effect (addr_of (at zap 0)) 5 1)
    (mb.fade 15)
    (while (not (mb.faded))
        (wait_cycles 17030)
        (set waited (+ waited 1)))
    (set still (if (at player.playing 0) 1 2))
    (mb.play (addr_of (at next 0)))
    (mb.fade_in 13)
    (for i u8 0 70
        (wait_cycles 17030))
    (mb.off))
)";

// the first tick from from on where voice v's volume is not want
int Changes(const Voices& v, int voice, int from, uint8_t want) {
    for (int t = from; t < static_cast<int>(v.volume[voice].size()); t++) {
        if (v.volume[voice][t] != want) {
            return t;
        }
    }
    return -1;
}

void FadeTests(TestResults& results) {
    MbBuild b;
    MbBuildText(results, "t.tsp", kFade, b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    tisple::RunApple2(*a, 200 * kMbTick);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    Voices v = ReadVoices(*a, FirstTick(*a), 170);
    // the fade's first step: voice 0 from 12 to 11
    int t = Changes(v, 0, 1, 12);
    CHECK(results, t > 5 && t < 20 && v.volume[0][t] == 11);
    if (t <= 5 || t >= 20) {
        return;
    }
    CHECK(results, v.volume[0][t - 1] == 12 && v.volume[3][t - 1] == 8);
    CHECK(results, v.volume[1][t - 1] == 16);
    // a step every 4 ticks, voice 3 three steps below voice 0, both silent at the end
    bool steps = true;
    for (int k = 0; k < 48; k++) {
        int down = k / 4 + 1;
        int want0 = 12 - down;
        int want3 = 8 - down;
        steps = steps && v.volume[0][t + k] == (want0 < 0 ? 0 : want0);
        steps = steps && v.volume[3][t + k] == (want3 < 0 ? 0 : want3);
    }
    CHECK(results, steps);
    // the effect on voice 5 at its own 15 all the while; the envelope's voice as it was until
    // the music stops (hush 60: 15 steps, t + 56), then quiet
    bool zap = true;
    for (int k = 0; k < 50; k++) {
        zap = zap && v.volume[5][t + k] == 15;
    }
    CHECK(results, zap);
    CHECK(results, v.volume[1][t + 54] == 16 && v.volume[1][t + 58] == 0);
    CHECK(results, a->cpu.mem[0x03C1] == 2); // stopped
    int waited = a->cpu.mem[0x03C0];
    CHECK(results, waited >= 55 && waited <= 62);
    // the next song: silent at 13 steps down, then a step every 4 ticks up to its 12
    uint16_t g3 = Period(Note('g', false, 3));
    int n = -1;
    for (int k = t + 56; k < static_cast<int>(v.period[0].size()); k++) {
        if (v.period[0][k] == g3) {
            n = k;
            break;
        }
    }
    CHECK(results, n > 0 && v.volume[0][n] == 0);
    if (n <= 0 || n + 56 >= static_cast<int>(v.volume[0].size())) {
        return;
    }
    int up = Changes(v, 0, n, 0);
    CHECK(results, up > n && up < n + 10 && v.volume[0][up] == 1);
    bool rises = true;
    for (int k = up; k < n + 56; k++) {
        int step = v.volume[0][k] - v.volume[0][k - 1];
        rises = rises && step >= 0 && step <= 1;
    }
    CHECK(results, rises && v.volume[0][n + 55] == 12);
    std::printf("mockingboard fade: silent and stopped after %d of the main loop's ticks\n",
                waited);
}

} // namespace

void RunMockingboardFadeTests(TestResults& results) {
    FadeTests(results);
}

} // namespace tisple_tests
