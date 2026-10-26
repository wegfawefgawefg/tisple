#pragma once

// The dynamic redundancy profiler. It watches every instruction the simulator runs and, per
// instruction address, counts executions and how often each kind of redundancy held:
// - CLC/SEC/CLV/CLD/CLI/SEI: the flag was already in that state
// - loads and transfers: the target register already held the value
// - stores: memory already held the value; separately, the byte was overwritten before
//   anything read it (counted when the overwrite happens)
// - branches: how often taken, and whether a CMP/CPX/CPY/BIT came just before
// - flag writers: every flag they wrote was overwritten before any was read
// - register writers: the register was overwritten before anything read it
// It also keeps what it saw on entry (flags, register ranges, the memory operand's range),
// which the superoptimizer can take as observed facts, and whether a stored value steps by
// the same amount every time (a strength-reduction hint).
//
// These are observations, not proofs: a condition that held on every execution of one run
// says a fact the compiler could know would make the instruction redundant.

#include "asm/opcodes.hpp"
#include "emu/machine.hpp"

#include <cstdint>
#include <vector>

namespace tisple {

struct SiteStats {
    std::uint64_t count = 0;  // executions
    std::uint64_t cycles = 0; // cycles they took (page crossings and taken branches included)
    std::uint64_t same = 0;   // the flag, register or byte already held the result
    std::uint64_t volatile_count = 0; // executions that touched $C000 and up: not judged
    std::uint64_t taken = 0;          // branches
    std::uint64_t after_compare = 0;  // branch executions right after a CMP/CPX/CPY/BIT
    std::uint16_t compare_site = 0;   // the last such compare
    std::uint64_t store_ends = 0;     // stores whose byte was later overwritten
    std::uint64_t store_dead = 0;     // of those, the byte was never read in between
    std::uint64_t flag_ends = 0;      // flag writes that ended (every flag it wrote overwritten)
    std::uint64_t flag_dead = 0;      // of those, none of its flags was read
    std::uint64_t reg_ends = 0;       // register writes that were overwritten
    std::uint64_t reg_dead = 0;       // of those, the register was never read in between
    // facts on entry
    std::uint8_t p_and = 0xFF;
    std::uint8_t p_or = 0;
    std::uint8_t a_min = 0xFF;
    std::uint8_t a_max = 0;
    std::uint8_t x_min = 0xFF;
    std::uint8_t x_max = 0;
    std::uint8_t y_min = 0xFF;
    std::uint8_t y_max = 0;
    std::uint8_t m_min = 0xFF; // the memory operand read (loads, ALU, RMW, compares)
    std::uint8_t m_max = 0;
    // stores: the value written, and how it moves from one execution to the next
    std::uint8_t last_value = 0;
    std::uint8_t last_step = 0;
    std::uint64_t step_repeats = 0; // executions whose step equalled the one before
};

struct FlagEvent {
    std::int32_t site = -1;
    std::uint8_t refs = 0; // flags still owned
    bool read = false;
};

// the instruction that just started; judged at the next instruction, when its effects are in
struct PendingIns {
    bool valid = false;
    std::uint16_t pc = 0;
    std::uint8_t opcode = 0;
    Regs before;
    bool has_ea = false;
    std::uint16_t ea = 0;
    std::uint8_t old_value = 0; // the byte at ea before it ran (below $C000 only)
    bool volatile_ea = false;   // ea at $C000 and up: i/o, rom or the language card
};

struct Profiler {
    std::vector<SiteStats> sites = std::vector<SiteStats>(65536);
    std::vector<BusCycle> bus; // the bus cycles of the pending instruction
    PendingIns pending;
    std::uint16_t last_pc = 0; // the instruction before the pending one
    Mn last_mn = Mn::NOP;
    // the store site that last wrote each byte (-1: something else) and whether it was read
    std::vector<std::int32_t> writer = std::vector<std::int32_t>(65536, -1);
    std::vector<std::uint8_t> byte_read = std::vector<std::uint8_t>(65536, 0);
    // N, Z, C, V: which event owns each flag; at most four are alive, plus the new one
    FlagEvent events[5];
    std::int8_t flag_owner[4] = {-1, -1, -1, -1};
    // A, X, Y: the instruction that last wrote each, and whether it has been read
    std::int32_t reg_owner[3] = {-1, -1, -1};
    bool reg_read[3] = {};
    std::uint64_t instructions = 0;
    std::uint64_t cycles = 0;
};

// before each instruction: judges the one that just ran, then notes this one. Pass as the
// harness's or the Apple II's trace function, with a Profiler as ctx; the machine's bus log
// must point at the profiler's `bus`.
void ProfileTrace(void* ctx, const Machine& m);
// judges the last instruction (call once the run stops)
void FinishProfile(Profiler& p, const Machine& m);
// a call from the harness came back: its caller reads the result in A and X
void ConsumeResult(Profiler& p);
// forget what is in flight (the machine was restarted); counts stay
void ResetProfileTracking(Profiler& p);

// one bit per flag in the masks below: N, Z, C, V as in the status register
std::uint8_t FlagsRead(Mn mn);
std::uint8_t FlagsWritten(Mn mn);

} // namespace tisple
