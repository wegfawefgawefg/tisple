// lib/mockingboard_bends on the machine with the emulated card: w (vibrato with its delay), p
// (glide from the note before), s+ and s- (slides, in a song and in an effect) and t (detune),
// read back a tick at a time from the sound chips' register writes and checked against the
// player's arithmetic. A program that calls service instead has none of the bends' lets;
// the song text's mistakes are errors that say what to write.

#include "link/image.hpp"
#include "link/layout.hpp"
#include "machine/mockingboard.hpp"
#include "test_mockingboard.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

const char* kBends = R"(
(import mockingboard mb)
(import mockingboard_song tune)
(tune.song bent
    (pattern vib "l20 v12 w2:10:4 c4 l60 r")
    (pattern glide "l20 v12 c4 p4 g4 l60 r")
    (pattern slide "l20 v12 s-3 c3 s+4 c5 l60 r")
    (pattern detuned "l20 v12 t-2 c4 t+3 c4 t0 c4 l40 r")
    (voice 0 vib)
    (voice 1 glide)
    (voice 2 slide)
    (voice 3 detuned))
(tune.effect laser "l12 v15 s-5 c6")
(fn irq () void :interrupt (mb.service_bends))
(fn main () void
    (mb.init)
    (mb.play (addr_of (at bent 0)))
    (wait_cycles 30000)
    (for i u8 0 20
        (wait_cycles 17030))
    (mb.effect (addr_of (at laser 0)) 4 1)
    (for i u8 0 60
        (wait_cycles 17030))
    (mb.off))
)";

// the vibrato's offset the player works out for a tick of phase ph and amount a
int Wobble(int ph, int a) {
    int off = ((ph + 0x20) & 0x40) == 0 ? a / 2 : a;
    return ph < 0x80 ? off : -off;
}

void BendsTests(TestResults& results, TickCost& cost) {
    MbBuild b;
    MbBuildText(results, "t.tsp", kBends, b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    cost.entry = b.code.irq_direct;
    tisple::RunApple2Traced(*a, 120 * kMbTick, MeasureTick, &cost);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    uint64_t first = FirstTick(*a);
    Voices v = ReadVoices(*a, first, 100);
    int c4 = Period(Note('c', false, 4));
    // voice 0, w2:10:4: the note's period for the note's tick and 4 more, then the wobble:
    // the depth 2 a shift of 6, the phase 256 / 10 a tick
    int amount = std::max(1, c4 >> 6);
    bool vib = true;
    int ph = 0;
    int lowest = c4;
    int highest = c4;
    for (int t = 0; t < 20; t++) {
        int want = c4;
        if (t >= 5) {
            want = c4 + Wobble(ph, amount);
            ph = (ph + 26) & 0xFF;
        }
        vib = vib && v.period[0][t] == want;
        lowest = std::min(lowest, static_cast<int>(v.period[0][t]));
        highest = std::max(highest, static_cast<int>(v.period[0][t]));
    }
    CHECK(results, vib);
    CHECK(results, lowest == c4 - amount && highest == c4 + amount);
    // voice 1: c4, then p4 g4 glides down from c4's period a sixteenth of g4's a tick
    int g4 = Period(Note('g', false, 4));
    int stride = g4 >> 4;
    bool glide = v.period[1][19] == c4 && v.period[1][20] == c4;
    int left = c4 - g4;
    for (int t = 21; t < 40; t++) {
        left = left > stride ? left - stride : 0;
        glide = glide && v.period[1][t] == g4 + left;
    }
    CHECK(results, glide && v.period[1][39] == g4);
    // voice 2: s-3 c3 lower each tick by c3's period >> 5, then s+4 c5 higher by c5's >> 4
    // down to period 0, where it stays
    int c3 = Period(Note('c', false, 3));
    int c5 = Period(Note('c', false, 5));
    bool slide = true;
    for (int t = 0; t < 20; t++) {
        slide = slide && v.period[2][t] == c3 + t * (c3 >> 5);
        int up = c5 - t * (c5 >> 4);
        slide = slide && v.period[2][20 + t] == (up > 0 ? up : 0);
    }
    CHECK(results, slide);
    // voice 3: t-2, t+3, t0
    CHECK(results, v.period[3][5] == c4 - 2 && v.period[3][25] == c4 + 3 && v.period[3][45] == c4);
    // the laser on voice 4: c6, then 7 lower a tick (c6's period >> 3) while it lasts
    int c6 = Period(Note('c', false, 6));
    int at = -1;
    for (int t = 0; t < 100 && at < 0; t++) {
        if (v.period[4][t] == c6 && v.volume[4][t] == 15) {
            at = t;
        }
    }
    CHECK(results, at > 0 && at < 80);
    if (at > 0 && at < 80) {
        bool laser = true;
        for (int k = 0; k < 12; k++) {
            laser = laser && v.period[4][at + k] == c6 + k * (c6 >> 3);
        }
        CHECK(results, laser);
    }
}

// the same song played by service: none of the bends' state is in the binary, and the code is
// smaller by what the bends' tick adds
void BytesTests(TestResults& results) {
    std::string lean_text = kBends;
    std::string bends_name = "(mb.service_bends)";
    lean_text.replace(lean_text.find(bends_name), bends_name.size(), "(mb.service)");
    MbBuild lean;
    MbBuildText(results, "t.tsp", lean_text, lean);
    MbBuild bends;
    MbBuildText(results, "t.tsp", kBends, bends);
    if (!lean.ok || !bends.ok) {
        return;
    }
    bool left_out = true;
    bool kept = true;
    for (const char* name : {"mockingboard_bends.goal", "mockingboard_bends.vib_rate",
                             "mockingboard_bends.detune", "mockingboard_bends.moving"}) {
        int32_t v = tisple::FindVar(lean.program, name);
        left_out = left_out && v >= 0 && lean.code.layout.vars[static_cast<size_t>(v)].dropped;
        int32_t w = tisple::FindVar(bends.program, name);
        kept = kept && w >= 0 && !bends.code.layout.vars[static_cast<size_t>(w)].dropped;
    }
    CHECK(results, left_out && kept);
    size_t without = lean.code.image.size();
    size_t with = bends.code.image.size();
    CHECK(results, with > without);
    std::printf("mockingboard: the bends' song with service %zu bytes, with service_bends %zu\n",
                without, with);
}

// the demo's song, which uses none of the commands, with service_bends: what the bends' tick
// costs a song that does not use them
void DemoCostTests(TestResults& results, TickCost& cost) {
    std::FILE* f = std::fopen("examples/mockingboard_demo.tsp", "rb");
    CHECK(results, f != nullptr);
    if (f == nullptr) {
        return;
    }
    std::string text;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) {
        text.append(buf, n);
    }
    std::fclose(f);
    size_t at = text.find("(mb.service)");
    CHECK(results, at != std::string::npos);
    if (at == std::string::npos) {
        return;
    }
    text.replace(at, 12, "(mb.service_bends)");
    MbBuild b;
    MbBuildText(results, "examples/mockingboard_demo.tsp", text, b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    tisple::ScheduleKey(*a, 300 * tisple::kCyclesPerFrame, '1');
    tisple::ScheduleKey(*a, 600 * tisple::kCyclesPerFrame, 0x1B);
    cost.entry = b.code.irq_direct;
    tisple::RunApple2Traced(*a, 700 * tisple::kCyclesPerFrame, MeasureTick, &cost);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, cost.count > 590);
}

// one voice's tick with service_bends: a long note held or wobbling, a note every tick plain
// or with vibrato and a glide; the average over 200 ticks
uint64_t OneVoiceTick(TestResults& results, const char* pattern) {
    std::string text = std::string(R"(
(import mockingboard mb)
(import mockingboard_song tune)
(tune.song one (pattern a ")") +
                       pattern + R"(") (voice 0 a))
(fn irq () void :interrupt (mb.service_bends))
(fn main () void
    (mb.init)
    (mb.play (addr_of (at one 0)))
    (while true))
)";
    MbBuild b;
    MbBuildText(results, "t.tsp", text, b);
    if (!b.ok) {
        return 0;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    TickCost cost;
    cost.entry = b.code.irq_direct;
    tisple::RunApple2Traced(*a, 210 * kMbTick, MeasureTick, &cost);
    CHECK(results, cost.count > 200);
    uint64_t total = 0;
    for (size_t k = 5; k < 205 && k < cost.each.size(); k++) {
        total += cost.each[k];
    }
    return total / 200;
}

void SlotCostTests(TestResults& results) {
    uint64_t held = OneVoiceTick(results, "l64 v12 c4");
    uint64_t wobbling = OneVoiceTick(results, "l64 v12 w4:10 c4");
    uint64_t plain = OneVoiceTick(results, "l1 v12 c4 d4");
    uint64_t bent = OneVoiceTick(results, "l1 v12 w2:10 p3 c4 d4");
    CHECK(results, wobbling > held && bent > plain);
    std::printf("mockingboard: one voice's tick with service_bends: a note held %llu cycles, "
                "wobbling %llu; a note each tick %llu, with w and p set %llu\n",
                static_cast<unsigned long long>(held), static_cast<unsigned long long>(wobbling),
                static_cast<unsigned long long>(plain), static_cast<unsigned long long>(bent));
}

struct BadSong {
    const char* text;
    const char* says;
};

// the format's checks on w, p, s and t
void ErrorTests(TestResults& results) {
    const BadSong bad[] = {
        {"w2 c4", "w wants depth:speed"},
        {"w5:10 c4", "w's depth is 5, not 0-4"},
        {"w2:1 c4", "w's speed is 1, not 2-64"},
        {"w0:4 c4", "w0 turns the vibrato off"},
        {"p8 c4", "p is 8, not 0-7"},
        {"s3 c4", "s wants a direction"},
        {"s+0 c4", "s0 turns the slide off"},
        {"t2 c4", "t wants a sign"},
        {"t+65 c4", "t is 65, not 0-64"},
    };
    for (const BadSong& song : bad) {
        std::string text = std::string("(import mockingboard_song tune)\n(tune.effect e \"") +
                           song.text + "\")\n(fn main () void)\n";
        tisple::LoadOptions load;
        load.files.push_back(tisple::VirtualFile{"t.tsp", text});
        tisple::Program program;
        std::vector<tisple::Diagnostic> errors;
        bool built = tisple::BuildProgram("t.tsp", load, program, errors);
        std::string said = tisple::FormatDiagnostics(program.sources, errors);
        CHECK(results, !built && said.find(song.says) != std::string::npos);
        if (built || said.find(song.says) == std::string::npos) {
            std::fprintf(stderr, "%s: %s\n", song.text, said.c_str());
        }
    }
    // and the good ones, as bytes
    const char* good = R"(
(import mockingboard_song tune)
(tune.effect e "w0 w4:64:255 w1:2 p0 p7 s0 s+1 s-7 t0 t+64 t-1 c4")
(fn main () void)
)";
    tisple::LoadOptions load;
    load.files.push_back(tisple::VirtualFile{"t.tsp", good});
    tisple::Program program;
    std::vector<tisple::Diagnostic> errors;
    bool built = tisple::BuildProgram("t.tsp", load, program, errors);
    CHECK(results, built);
    int32_t e = tisple::FindVar(program, "t.e");
    CHECK(results, e >= 0);
    if (!built || e < 0) {
        std::fprintf(stderr, "%s", tisple::FormatDiagnostics(program.sources, errors).c_str());
        return;
    }
    const std::vector<uint8_t> want = {0xE6, 0,    0,    0,    0xE6, 4,    4,    255,  0xE6,
                                       7,    128,  0,    0xE7, 0,    0xE7, 0x11, 0xE7, 0,
                                       0xE7, 0x27, 0xE7, 0x31, 0xE8, 0,    0,    0xE8, 64,
                                       0,    0xE8, 0xFF, 0xFF, 36,   0xFF};
    tisple::Layout layout;
    CHECK(results, tisple::LayoutProgram(program, tisple::LayoutOptions(), layout, errors));
    CHECK(results, tisple::InitBytes(program, layout, e) == want);
}

} // namespace

void RunMockingboardBendsTests(TestResults& results) {
    TickCost cost;
    BendsTests(results, cost);
    BytesTests(results);
    ErrorTests(results);
    SlotCostTests(results);
    TickCost demo;
    DemoCostTests(results, demo);
    if (cost.count == 0 || demo.count == 0) {
        return;
    }
    std::vector<uint64_t> sorted = demo.each;
    std::sort(sorted.begin(), sorted.end());
    std::printf("mockingboard: a tick with w p s t playing %llu cycles on average, %llu at most; "
                "the demo with service_bends %llu on average, %llu the median, %llu at most\n",
                static_cast<unsigned long long>(cost.total / cost.count),
                static_cast<unsigned long long>(cost.most),
                static_cast<unsigned long long>(demo.total / demo.count),
                static_cast<unsigned long long>(sorted[sorted.size() / 2]),
                static_cast<unsigned long long>(demo.most));
}

} // namespace tisple_tests
