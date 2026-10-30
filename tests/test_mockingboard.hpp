#pragma once

// what the lib/mockingboard tests share (test_mockingboard_support.cpp): a program built with
// the real lib/, the machine started with the card, the cycles each interrupt takes, and the
// sound chips' registers read back a tick at a time

#include "test_backend.hpp"
#include "test_machine.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tisple_tests {

constexpr uint64_t kMbTick = 17030; // the lib's default rate

struct MbBuild {
    tisple::Program program;
    tisple::Compiled code;
    bool ok = false;
};

// path's program (text, or the file when text is empty) built with the real lib/
void MbBuildText(TestResults& results, const std::string& path, const std::string& text,
                 MbBuild& b);

uint16_t MbVarAddress(const MbBuild& b, const char* name);

// loaded, the card in slot (0 for none), the IRQ vector at $FFFE in the card's ram straight to
// the program's entry, as with the card's ram in: no rom needed
std::unique_ptr<tisple::Apple2> MbStart(const MbBuild& b, int slot);

// the cycles each interrupt takes, from the cpu's 7-cycle entry to the end of its RTI
struct TickCost {
    uint16_t entry = 0;
    uint64_t began = 0;
    bool inside = false;
    uint64_t count = 0;
    uint64_t total = 0;
    uint64_t most = 0;
    std::vector<uint64_t> each;
};

void MeasureTick(void* ctx, const tisple::Machine& m);

// a chip's registers at each tick: the writes replayed, looked at in the middle of each tick
struct Voices {
    std::vector<uint16_t> period[6]; // a tick each
    std::vector<uint8_t> volume[6];
    std::vector<uint8_t> mixer[2];
};

Voices ReadVoices(const tisple::Apple2& a, uint64_t first_tick, int ticks);

// what the lib's NOTE_PERIOD holds for a note name's number (C1 = 0)
uint16_t Period(int note);

int Note(char letter, bool sharp, int octave);

// the first tick: the first interrupt's cycle, from the first write of voice 1's tone
uint64_t FirstTick(const tisple::Apple2& a);

} // namespace tisple_tests
