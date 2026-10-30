#include "fuzz_gen_internal.hpp"

// An interrupt handler for the seed's program (src/check/interrupts.cpp, src/backend/irq.cpp):
// it works on globals of its own (hq, hw, ha, hs), which main never names, with multiplies and
// divisions by variables (the runtime's scratch), indexed reads and writes (X and Y), a loop,
// a helper with a frame of its own; main now and then turns interrupts off around a statement.
// The fuzzer raises interrupts early in main on the 6502 and in the interpreter: what the
// handler leaves depends only on how many it took, and main's globals not at all, so every
// global must come out the same. A handler that clobbers a register, a scratch byte or a frame
// of the code it stops shows as a difference.

namespace tisple_tests::fuzz {

namespace {

const char* const kOps[] = {"+", "-", "*", "/", "%", "&", "|", "^"};

} // namespace

// a u8 or u16 value from the handler's own variables
std::string Gen::IrqValue(bool two, int depth) {
    int pick = irq_rng.Below(depth > 0 ? 7 : 4);
    std::string leaf;
    switch (pick) {
    case 0:
        return std::to_string(two ? irq_rng.Below(65536) : irq_rng.Below(256));
    case 1:
        leaf = "hq" + std::to_string(irq_rng.Below(3));
        return two ? "(as u16 " + leaf + ")" : leaf;
    case 2:
        leaf = "hw" + std::to_string(irq_rng.Below(2));
        return two ? leaf : "(as u8 " + leaf + ")";
    case 3:
        leaf = "(at ha (& " + IrqValue(false, 0) + " 15))";
        return two ? "(as u16 " + leaf + ")" : leaf;
    case 4: {
        std::string call =
            "(hf " + IrqValue(false, depth - 1) + " " + IrqValue(true, depth - 1) + ")";
        return two ? "(as u16 " + call + ")" : call;
    }
    default: {
        // a literal on both sides would have no type: a variable on the left
        std::string left = "hq" + std::to_string(irq_rng.Below(3));
        if (two) {
            left = irq_rng.Chance(50) ? "hw" + std::to_string(irq_rng.Below(2)) : "hs";
        }
        return "(" + std::string(kOps[irq_rng.Below(8)]) + " " + left + " " +
               IrqValue(two, depth - 1) + ")";
    }
    }
}

std::string Gen::IrqStmt(int depth) {
    switch (irq_rng.Below(depth > 0 ? 6 : 4)) {
    case 0: {
        std::string q = "hq" + std::to_string(irq_rng.Below(3));
        return "(set " + q + " " + IrqValue(false, 2) + ")";
    }
    case 1: {
        std::string w = "hw" + std::to_string(irq_rng.Below(2));
        return "(set " + w + " (+ " + w + " " + IrqValue(true, 2) + "))";
    }
    case 2:
        return "(set (at ha (& " + IrqValue(false, 1) + " 15)) " + IrqValue(false, 2) + ")";
    case 3:
        return "(set hs (+ hs 1))";
    case 4:
        return "(for hj u8 0 (& " + IrqValue(false, 1) +
               " 7)\n        (set (at ha hj) (+ (at ha hj) " + IrqValue(false, 1) + ")))";
    default:
        return "(if (< hq" + std::to_string(irq_rng.Below(3)) + " " + IrqValue(false, 1) + ") " +
               IrqStmt(0) + " " + IrqStmt(0) + ")";
    }
}

std::string Gen::IrqHelpers() {
    std::string text = "(let hq0 u8 " + std::to_string(irq_rng.Below(256)) + ")\n";
    text += "(let hq1 u8 " + std::to_string(irq_rng.Below(256)) + ")\n";
    text += "(let hq2 u8 0)\n";
    text += "(let hw0 u16 " + std::to_string(irq_rng.Below(65536)) + ")\n";
    text += "(let hw1 u16 0)\n(let hs u16 0)\n(let ha (array u8 16))\n";
    // a helper with a local of its own, which only the handler calls
    text += "(fn hf ((a u8) (b u16)) u8\n    (let t u16 (+ (* b (as u16 a)) " +
            std::to_string(1 + irq_rng.Below(300)) + "))\n    (as u8 (/ t (| (as u16 a) 1))))\n";
    text += "(fn on_irq () void :interrupt\n    (set hs (+ hs 1))";
    int count = 2 + irq_rng.Below(4);
    for (int i = 0; i < count; i++) {
        text += "\n    " + IrqStmt(1);
    }
    return text + ")\n";
}

// a statement of main's with interrupts off around it
std::string Gen::IrqMask(int depth) {
    // what the statement declares ends with the do
    scopes.emplace_back();
    std::string stmt = Stmt(depth - 1);
    scopes.pop_back();
    DropPointers();
    return "(do\n    (sei)\n    " + stmt + "\n    (cli))";
}

} // namespace tisple_tests::fuzz
