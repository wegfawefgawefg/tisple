#include "emu/harness.hpp"

#include "emu/disasm.hpp"

namespace tisple {

const char* StopReasonName(StopReason reason) {
    switch (reason) {
    case StopReason::kStopAddress:
        return "stop address reached";
    case StopReason::kBrk:
        return "BRK";
    case StopReason::kIllegalOpcode:
        return "illegal opcode";
    default:
        return "cycle limit";
    }
}

void LoadBytes(Machine& m, uint16_t addr, const uint8_t* bytes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        m.mem[static_cast<uint16_t>(addr + i)] = bytes[i];
    }
}

RunResult Run(Machine& m, const RunOptions& options) {
    RunResult result;
    uint64_t start_cycles = m.cycles;
    for (;;) {
        uint16_t pc = M6502_GET_ADDR(m.pins);
        uint8_t opcode = m.mem[pc];
        if (static_cast<int32_t>(pc) == options.stop_pc) {
            result.reason = StopReason::kStopAddress;
        } else if (opcode == 0x00) {
            result.reason = StopReason::kBrk;
        } else if (!IsDocumentedOpcode(opcode)) {
            result.reason = StopReason::kIllegalOpcode;
        } else if (m.cycles - start_cycles >= options.max_cycles) {
            result.reason = StopReason::kCycleLimit;
        } else {
            if (options.trace != nullptr) {
                options.trace(options.trace_ctx, m);
            }
            StepInstruction(m);
            continue;
        }
        break;
    }
    result.regs = GetRegs(m);
    result.cycles = m.cycles - start_cycles;
    return result;
}

} // namespace tisple
