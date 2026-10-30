#pragma once

// The runtime routines' writer: instructions and labels as assembler items (runtime.cpp,
// runtime_div.cpp). Same license note as runtime.hpp.

#include "backend/gen.hpp"

#include <string>

namespace tisple {

struct Writer {
    Backend* be = nullptr;
    std::vector<AsmItem>* out = nullptr;
    // second entries of the unsigned divisions, for the signed ones, divu16 and divisions that
    // compare inline (the dividend at least the divisor, divu8's also in A; neither reads the
    // flags it is entered with)
    std::int32_t divu8_ge = -1;
    std::int32_t divu16_ge = -1;

    void Op(Mn mn, Mode mode = Mode::Imp, AsmValue value = AsmValue()) {
        out->push_back(MakeIns(mn, mode, value));
    }
    void Mem(Mn mn, std::int32_t sym, std::int32_t offset = 0) {
        Op(mn, Mode::Abs, SymValue(sym, offset));
    }
    void Imm(Mn mn, std::int32_t value) {
        Op(mn, Mode::Imm, Num(value));
    }
    void Br(Mn mn, std::int32_t label) {
        Op(mn, Mode::Rel, SymValue(label));
    }
    void Jmp(std::int32_t label) {
        Op(Mn::JMP, Mode::Abs, SymValue(label));
    }
    std::int32_t Label(const std::string& name) {
        return NewSym(*be, name, SymRole::Label, -1);
    }
    void Place(std::int32_t label) {
        out->push_back(MakeLabel(label));
    }
    // 16-bit negate in place
    void Negate16(std::int32_t sym) {
        Imm(Mn::LDA, 0);
        Op(Mn::SEC);
        Mem(Mn::SBC, sym);
        Mem(Mn::STA, sym);
        Imm(Mn::LDA, 0);
        Mem(Mn::SBC, sym, 1);
        Mem(Mn::STA, sym, 1);
    }
    void Negate8A() {
        Imm(Mn::EOR, 0xFF);
        Op(Mn::CLC);
        Imm(Mn::ADC, 1);
    }
};

// the division routines (runtime_div.cpp), each placed after its label by EmitRuntime
void DivU8(Writer& w);
void DivU16(Writer& w);
void DivS8(Writer& w);
void DivS16(Writer& w);

} // namespace tisple
