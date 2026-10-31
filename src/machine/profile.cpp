#include "machine/profile.hpp"

#include <algorithm>
#include <cstdio>

namespace tisple {

namespace {

enum Where : uint32_t { kMain, kAux, kCard1, kCard2, kAuxCard1, kAuxCard2, kRom, kWheres };

const char* const kWhereNames[kWheres] = {"main",     "aux",      "card1", "card2",
                                          "auxcard1", "auxcard2", "rom"};

// the memory an opcode at pc is fetched from, by the switches
Where WhereFetched(const Apple2& a, uint16_t pc) {
    if (pc < 0x0200) {
        return a.sw.altzp ? kAux : kMain;
    }
    if (pc < 0xC000) {
        return a.sw.ramrd ? kAux : kMain;
    }
    if (pc < 0xD000 || !a.lc.read_ram) {
        return kRom;
    }
    bool bank1 = pc < 0xE000 && !a.lc.bank2;
    if (a.sw.altzp) {
        return bank1 ? kAuxCard1 : kAuxCard2;
    }
    return bank1 ? kCard1 : kCard2;
}

} // namespace

void StartProfile(Profile& profile, const Apple2& a, uint64_t from) {
    profile.apple = &a;
    profile.from = from;
    profile.cycles.assign(static_cast<size_t>(kWheres) * 65536, 0);
    profile.counting = false;
}

// each instruction's cycles are counted when the next one starts
void ProfileStep(void* ctx, const Machine& m) {
    Profile& profile = *static_cast<Profile*>(ctx);
    if (profile.counting) {
        profile.cycles[profile.place] += m.cycles - profile.began;
    }
    profile.counting = m.cycles >= profile.from;
    if (profile.counting) {
        uint16_t pc = M6502_GET_ADDR(m.pins);
        profile.place = WhereFetched(*profile.apple, pc) * 65536 + pc;
        profile.began = m.cycles;
    }
}

bool WriteProfile(const Profile& profile, const char* path, std::string& error) {
    std::vector<uint32_t> places;
    for (uint32_t place = 0; place < profile.cycles.size(); place++) {
        if (profile.cycles[place] > 0) {
            places.push_back(place);
        }
    }
    std::sort(places.begin(), places.end(), [&](uint32_t x, uint32_t y) {
        return profile.cycles[x] > profile.cycles[y];
    });
    FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        error = std::string("cannot write '") + path + "'";
        return false;
    }
    for (uint32_t place : places) {
        std::fprintf(file, "%s %04X %llu\n", kWhereNames[place / 65536], place % 65536,
                     static_cast<unsigned long long>(profile.cycles[place]));
    }
    return std::fclose(file) == 0;
}

} // namespace tisple
