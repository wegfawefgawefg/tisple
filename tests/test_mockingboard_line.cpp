// lib/mockingboard's effects in line: several fired in one frame play one after another, each
// on its own voice, with no tick between them; one with a higher priority cuts off the one
// playing and the others wait after it. And the speaker's version of an effect is only in a
// program that calls it.

#include "machine/mockingboard.hpp"
#include "test_mockingboard.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

const char* kLine = R"(
(import mockingboard mb)
(import mockingboard_song tune)
(tune.song hum (pattern a "l64 v8 c3") (voice 0 a))
(tune.effect one "l2 v15 c6 d6")
(tune.effect two "l2 v15 e6 f6")
(tune.effect three "l2 v15 g6 a6")
(tune.effect loud "l2 v15 b6 c7")
(let idle u8 0 :address 0x03C0) ; outside zero page, which the end puts back
(fn irq () void :interrupt (mb.service))
(fn main () void
    (mb.init)
    (mb.play (addr_of (at hum 0)))
    (wait_cycles 40000)
    (mb.effect (addr_of (at one 0)) 5 1)
    (mb.effect (addr_of (at two 0)) 2 1)
    (mb.effect (addr_of (at three 0)) 5 1)
    (for i u8 0 20
        (wait_cycles 17030))
    (set idle (if (mb.effect_playing) 1 2))
    (mb.effect (addr_of (at one 0)) 5 1)
    (mb.effect (addr_of (at two 0)) 5 1)
    (wait_cycles 30000)
    (mb.effect (addr_of (at loud 0)) 5 9)
    (for i u8 0 20
        (wait_cycles 17030))
    (mb.off))
)";

// the first tick: the hum's note on voice 0
uint64_t HumTick(const tisple::Apple2& a) {
    const tisple::MockingboardState& mb = a.slots.mockingboard;
    for (uint64_t n = 0; n < mb.log_count; n++) {
        const tisple::PsgWrite& w = tisple::MockingboardWrite(a, n);
        if (w.chip == 0 && w.reg == 0 && w.value != 0) {
            return w.cycle - 2000;
        }
    }
    return 0;
}

// the first tick from from on where voice v plays the note
int Find(const Voices& v, int voice, int from, uint16_t period) {
    for (int t = from < 0 ? 0 : from; t < static_cast<int>(v.period[voice].size()); t++) {
        if (v.period[voice][t] == period && v.volume[voice][t] != 0) {
            return t;
        }
    }
    return -1;
}

// voice plays period for ticks ticks from t
bool Holds(const Voices& v, int voice, int t, int ticks, uint16_t period) {
    for (int k = t; k < t + ticks; k++) {
        if (k < 0 || k >= static_cast<int>(v.period[voice].size()) ||
            v.period[voice][k] != period || v.volume[voice][k] != 15) {
            return false;
        }
    }
    return true;
}

void LineTests(TestResults& results) {
    MbBuild b;
    MbBuildText(results, "t.tsp", kLine, b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    tisple::RunApple2(*a, 120 * kMbTick);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    Voices v = ReadVoices(*a, HumTick(*a), 100);
    uint16_t c6 = Period(Note('c', false, 6));
    uint16_t d6 = Period(Note('d', false, 6));
    uint16_t e6 = Period(Note('e', false, 6));
    uint16_t f6 = Period(Note('f', false, 6));
    uint16_t g6 = Period(Note('g', false, 6));
    uint16_t a6 = Period(Note('a', false, 6));
    uint16_t b6 = Period(Note('b', false, 6));
    uint16_t c7 = Period(Note('c', false, 7));
    // one, two and three fired together: one after another, no tick between
    int t = Find(v, 5, 0, c6);
    CHECK(results, t > 0 && t < 40);
    if (t <= 0 || t >= 40) {
        return;
    }
    CHECK(results, Holds(v, 5, t, 2, c6) && Holds(v, 5, t + 2, 2, d6));
    CHECK(results, Holds(v, 2, t + 4, 2, e6) && Holds(v, 2, t + 6, 2, f6));
    CHECK(results, Holds(v, 5, t + 8, 2, g6) && Holds(v, 5, t + 10, 2, a6));
    // each voice back to the music (none on 2 and 5: quiet) the tick after
    CHECK(results, v.volume[5][t + 5] == 0 && v.volume[2][t + 9] == 0 && v.volume[5][t + 13] == 0);
    CHECK(results, a->cpu.mem[0x03C0] == 2); // nothing playing or waiting after
    // one again, two waiting; loud cuts one off, two plays after loud
    int again = Find(v, 5, t + 14, c6);
    int cut = Find(v, 5, again, b6);
    CHECK(results, again > 0 && cut > again && cut < again + 4);
    if (again <= 0 || cut <= again || cut >= again + 4) {
        return;
    }
    CHECK(results, Holds(v, 5, cut, 2, b6) && Holds(v, 5, cut + 2, 2, c7));
    CHECK(results, Holds(v, 5, cut + 4, 2, e6) && Holds(v, 5, cut + 6, 2, f6));
    // the music under it all
    bool hum = true;
    for (int k = 0; k < cut + 8 && k < static_cast<int>(v.period[0].size()); k++) {
        hum = hum && v.period[0][k] == Period(Note('c', false, 3));
    }
    CHECK(results, hum);
}

// the same program with and without the speaker's version of its effect
const char* kCardOnly = R"(
(import mockingboard mb)
(import mockingboard_song tune)
(tune.effect zap "l1 v15 c7 g6 d6 a5")
(fn irq () void :interrupt (mb.service))
(fn main () void
    (mb.init)
    (if mb.found
        (mb.effect (addr_of (at zap 0)) 2 1))
    (mb.off))
)";

const char* kWithSpeaker = R"(
(import mockingboard mb)
(import mockingboard_song tune)
(tune.effect zap "l1 v15 c7 g6 d6 a5")
(fn irq () void :interrupt (mb.service))
(fn main () void
    (mb.init)
    (if mb.found
        (mb.effect (addr_of (at zap 0)) 2 1)
        (mb.speaker_effect (addr_of (at zap 0))))
    (mb.off))
)";

void SpeakerBytesTests(TestResults& results) {
    MbBuild card;
    MbBuildText(results, "t.tsp", kCardOnly, card);
    MbBuild both;
    MbBuildText(results, "t.tsp", kWithSpeaker, both);
    if (!card.ok || !both.ok) {
        return;
    }
    size_t without = card.code.image.size();
    size_t with = both.code.image.size();
    CHECK(results, with > without + 600);
    std::printf("mockingboard: a program with card effects only %zu bytes, with the speaker's "
                "too %zu\n",
                without, with);
}

} // namespace

void RunMockingboardLineTests(TestResults& results) {
    LineTests(results);
    SpeakerBytesTests(results);
}

} // namespace tisple_tests
