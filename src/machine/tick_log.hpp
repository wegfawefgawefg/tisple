#pragma once

// `tisple machine --ticks F.txt`: every interrupt the cpu takes from a device's line (the
// Mockingboard's 6522s), one line each:
//   taken late held where
// taken the cycle count the cpu took it, late the cycles the line had been low by then (a tick
// later than its timer's period has lost one), held the address of the instruction the cpu was
// at when the line went low, and where the memory it was fetched from (as --profile names it).
// A tick held off by interrupts off is late; held then says which code had them off.

#include "machine/apple2.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

struct TickEntry {
    uint64_t taken = 0;
    uint64_t late = 0;
    uint16_t held = 0;
    uint8_t where = 0;
};

struct TickLog {
    const Apple2* apple = nullptr;
    uint64_t seen_taken = 0;      // the cpu's count of interrupts taken at the last step
    uint64_t low_at = UINT64_MAX; // the line's fall the next entry is for
    uint64_t last_taken = 0;
    uint16_t held = 0;
    uint8_t where = 0;
    std::vector<TickEntry> ticks;
};

void StartTickLog(TickLog& log, const Apple2& a);
// a MachineTraceFn: ctx is the TickLog
void TickLogStep(void* ctx, const Machine& m);
bool WriteTickLog(const TickLog& log, const char* path, std::string& error);
// "ticks: 600, the latest 11,695 cycles late"
std::string DescribeTickLog(const TickLog& log);

// the memory an opcode at pc is fetched from, by the switches, and its name (profile.cpp)
uint8_t FetchedFrom(const Apple2& a, uint16_t pc);
const char* FetchedFromName(uint8_t where);

} // namespace tisple
