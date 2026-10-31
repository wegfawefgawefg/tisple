#pragma once

// `tisple machine --profile F.txt[@C]`: from cycle C (default the start) to the end of the run,
// the cycles each instruction took, by its address and the memory it was fetched from (main,
// aux, the language cards' banks, rom). F.txt has one line a place, most cycles first:
//   where address cycles
// where is main, aux, card1, card2 (the language card's $D000 banks; card2 also for $E000 on),
// auxcard1, auxcard2 or rom. tools/profile.py adds them up by the program's functions and lines.

#include "machine/apple2.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

struct Profile {
    const Apple2* apple = nullptr;
    uint64_t from = 0;
    std::vector<uint64_t> cycles; // by place: where * 65536 + address
    bool counting = false;        // an instruction is in flight: its place and start below
    uint32_t place = 0;
    uint64_t began = 0;
};

void StartProfile(Profile& profile, const Apple2& a, uint64_t from);
// a MachineTraceFn: ctx is the Profile
void ProfileStep(void* ctx, const Machine& m);
bool WriteProfile(const Profile& profile, const char* path, std::string& error);

} // namespace tisple
