#pragma once

#include "emu/machine.hpp"

#include <cstddef>
#include <cstdint>

namespace tisple {

enum class StopReason {
    kStopAddress,
    kBrk,
    kIllegalOpcode,
    kCycleLimit,
};

// called before each instruction; the registers' pc is the instruction about to run
using TraceFn = void (*)(void* ctx, const Machine& m);

struct RunOptions {
    int32_t stop_pc = -1;             // stop when PC reaches this address; -1 for none
    uint64_t max_cycles = UINT64_MAX; // counted from the start of this run
    TraceFn trace = nullptr;
    void* trace_ctx = nullptr;
};

struct RunResult {
    StopReason reason = StopReason::kCycleLimit;
    Regs regs;
    uint64_t cycles = 0; // used by this run
};

const char* StopReasonName(StopReason reason);

void LoadBytes(Machine& m, uint16_t addr, const uint8_t* bytes, size_t count);

// runs from the current registers; stops before executing BRK or an undocumented opcode
RunResult Run(Machine& m, const RunOptions& options);

} // namespace tisple
