#pragma once

// a thin writer over src/asm for the small hand-written 6502 routines in the disk code (our
// disk ii boot rom and the boot loader): one call per instruction, labels by name.
// BuildCode refuses code whose branches would need relaxing, since these routines count
// cycles and must not change shape behind our back.

#include "asm/asm.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

struct Code6502 {
    Assembly as;

    std::int32_t Label(const char* name) {
        return NewSymbol(as, name);
    }
    void Place(std::int32_t label) {
        as.items.push_back(MakeLabel(label));
    }
    void Op(Mn mn) {
        as.items.push_back(MakeIns(mn, Mode::Imp));
    }
    void OpA(Mn mn) {
        as.items.push_back(MakeIns(mn, Mode::Acc));
    }
    void Imm(Mn mn, int value) {
        as.items.push_back(MakeIns(mn, Mode::Imm, Num(value & 0xFF)));
    }
    // absolute, or zero page when the address is below $100
    void Mem(Mn mn, int addr) {
        as.items.push_back(MakeIns(mn, Mode::Abs, Num(addr)));
    }
    void MemX(Mn mn, int addr) {
        as.items.push_back(MakeIns(mn, addr < 0x100 ? Mode::ZpX : Mode::AbsX, Num(addr)));
    }
    void MemY(Mn mn, int addr) {
        as.items.push_back(MakeIns(mn, Mode::AbsY, Num(addr)));
    }
    void IndY(Mn mn, int zp) {
        as.items.push_back(MakeIns(mn, Mode::IndY, Num(zp)));
    }
    void Jump(Mn mn, std::int32_t label) { // JMP or JSR to a label
        as.items.push_back(MakeIns(mn, Mode::Abs, SymValue(label)));
    }
    // the operand is a label's address (a self-modified instruction's operand, say)
    void MemLabel(Mn mn, std::int32_t label, int offset = 0) {
        as.items.push_back(MakeIns(mn, Mode::Abs, SymValue(label, offset)));
    }
    // a table in the code, indexed by X
    void MemLabelX(Mn mn, std::int32_t label, int offset = 0) {
        as.items.push_back(MakeIns(mn, Mode::AbsX, SymValue(label, offset)));
    }
    void Br(Mn mn, std::int32_t label) {
        as.items.push_back(MakeIns(mn, Mode::Rel, SymValue(label)));
    }
    void Byte(int value) {
        as.items.push_back(MakeByte(Num(value & 0xFF)));
    }
};

inline bool BuildCode(Code6502& code, std::uint16_t origin, std::vector<std::uint8_t>& out,
                      std::string& error) {
    LayoutCode(code.as, origin);
    for (const AsmItem& item : code.as.items) {
        if (item.long_branch) {
            error = "a branch is out of range: " + ItemText(code.as, item);
            return false;
        }
    }
    return EncodeCode(code.as, out, error);
}

// a label's address after BuildCode
inline std::uint16_t LabelAddress(const Code6502& code, std::int32_t label) {
    return static_cast<std::uint16_t>(code.as.symbols[static_cast<std::size_t>(label)].value);
}

} // namespace tisple
