// lib/joystick on the built-in machine: both axes from one read against the paddles' values,
// the single paddle read, the buttons, dir, and what a read costs; the scripted --joystick

#include "machine/scripted_input.hpp"
#include "test_apple2_libs.hpp"
#include "test_mockingboard.hpp"

#include <cstdio>
#include <cstdlib>

namespace tisple_tests {

namespace {

// main reads everything once into page 3, then returns
const char* const kReadAll = "(import joystick)\n"
                             "(let out (array u8 12) :address 0x0300)\n"
                             "(fn main () void\n"
                             "    (joystick.read)\n"
                             "    (set (at out 0) joystick.x)\n"
                             "    (set (at out 1) joystick.y)\n"
                             "    (set (at out 2) (if joystick.button0 1 0))\n"
                             "    (set (at out 3) (if joystick.button1 1 0))\n"
                             "    (set (at out 4) (joystick.paddle 0))\n"
                             "    (wait_cycles 3500)\n"
                             "    (set (at out 5) (joystick.paddle 1))\n"
                             "    (set (at out 6) (as u8 (joystick.dir joystick.x)))\n"
                             "    (set (at out 7) (as u8 (joystick.dir joystick.y)))\n"
                             "    (set (at out 8) (if (joystick.button 2) 1 0))\n"
                             "    (wait_cycles 3500)\n"
                             "    (joystick.read_pair 2)\n"
                             "    (set (at out 9) joystick.x)\n"
                             "    (set (at out 10) joystick.y))\n";

struct Reading {
    int out[12] = {};
    uint64_t cycles = 0;
};

Reading RunWith(const MbBuild& b, const uint8_t paddles[4], const bool buttons[3]) {
    Reading r;
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 0);
    for (int i = 0; i < 4; i++) {
        tisple::SetPaddle(*a, i, paddles[i]);
    }
    for (int i = 0; i < 3; i++) {
        tisple::SetButton(*a, i, buttons[i]);
    }
    tisple::RunApple2(*a, 1000000);
    if (a->stop_reason != tisple::MachineStop::kReturned) {
        r.out[0] = -1;
        return r;
    }
    for (int i = 0; i < 12; i++) {
        r.out[i] = a->cpu.mem[0x0300 + i];
    }
    r.cycles = tisple::Cycles(*a);
    return r;
}

bool Near(int got, int want) {
    return std::abs(got - want) <= 1;
}

// every value on each axis, the other axis at the mirror value so either ends first
void TestAxes(TestResults& results, const MbBuild& b) {
    const bool none[3] = {false, false, false};
    int far = 0;
    for (int v = 0; v < 256; v++) {
        uint8_t paddles[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(255 - v),
                              static_cast<uint8_t>((v * 7) & 0xFF), static_cast<uint8_t>(v / 2)};
        Reading r = RunWith(b, paddles, none);
        bool ok = r.out[0] >= 0 && Near(r.out[0], v) && Near(r.out[1], 255 - v) &&
                  Near(r.out[4], v) && Near(r.out[5], 255 - v) && Near(r.out[9], paddles[2]) &&
                  Near(r.out[10], paddles[3]);
        if (!ok && far++ < 4) {
            std::fprintf(stderr, "joystick %d: read %d %d, paddle %d %d, pair 2: %d %d\n", v,
                         r.out[0], r.out[1], r.out[4], r.out[5], r.out[9], r.out[10]);
        }
    }
    CHECK(results, far == 0);
}

void TestButtonsAndDir(TestResults& results, const MbBuild& b) {
    uint8_t left_down[4] = {10, 250, 0, 0};
    bool b0[3] = {true, false, false};
    Reading r = RunWith(b, left_down, b0);
    CHECK(results, r.out[2] == 1 && r.out[3] == 0 && r.out[8] == 0);
    CHECK(results, r.out[6] == 0xFF && r.out[7] == 1); // left, down
    uint8_t centre[4] = {127, 127, 127, 127};
    bool b12[3] = {false, true, true};
    r = RunWith(b, centre, b12);
    CHECK(results, r.out[2] == 0 && r.out[3] == 1 && r.out[8] == 1);
    CHECK(results, r.out[6] == 0 && r.out[7] == 0);
}

// what one read costs: the longer axis's time, 2,816 cycles more at 255 than at 0
void TestCost(TestResults& results) {
    MbBuild b;
    MbBuildText(results, "t.tsp",
                "(import joystick)\n"
                "(fn main () void\n"
                "    (joystick.read))\n",
                b);
    if (!b.ok) {
        return;
    }
    const bool none[3] = {false, false, false};
    uint8_t low[4] = {0, 0, 0, 0};
    uint8_t high[4] = {255, 20, 0, 0};
    uint64_t base = RunWith(b, low, none).cycles;
    uint64_t full = RunWith(b, high, none).cycles;
    std::printf("joystick: a read at 255 takes %llu cycles more than at 0\n",
                static_cast<unsigned long long>(full - base));
    CHECK(results, full - base > 2700 && full - base < 2950);
}

void TestScripted(TestResults& results) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    std::string error;
    CHECK(results, tisple::ScheduleJoystickInput(*a, "10:0,255,3;5:200,100,0", error));
    CHECK(results, !tisple::ScheduleJoystickInput(*a, "10:0,256,3", error));
    CHECK(results, !tisple::ScheduleJoystickInput(*a, "10:0,25,8", error));
    tisple::RunApple2(*a, 6); // nothing to run: the machine stops, but the events apply
    std::vector<uint8_t> loop = {0x4C, 0x00, 0x60};
    std::unique_ptr<tisple::Apple2> m = MachineWithBytes(loop);
    CHECK(results, tisple::ScheduleJoystickInput(*m, "100:0,255,3;50:200,100,0", error));
    tisple::RunApple2(*m, 75);
    CHECK(results, m->game.paddle[0] == 200 && m->game.paddle[1] == 100 && !m->game.button[0]);
    tisple::RunApple2(*m, 100);
    CHECK(results, m->game.paddle[0] == 0 && m->game.paddle[1] == 255);
    CHECK(results, m->game.button[0] && m->game.button[1] && !m->game.button[2]);
}

} // namespace

void RunJoystickLibTests(TestResults& results) {
    MbBuild b;
    MbBuildText(results, "t.tsp", kReadAll, b);
    if (b.ok) {
        TestAxes(results, b);
        TestButtonsAndDir(results, b);
    }
    TestCost(results);
    TestScripted(results);
}

} // namespace tisple_tests
