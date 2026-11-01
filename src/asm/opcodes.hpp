#pragma once

// The 151 documented NMOS 6502 opcodes, and the 65C02's 27 more (asm/cpu.hpp: Apple's 65C02,
// no Rockwell bit instructions): mnemonic and addressing mode to opcode byte, with the
// published cycle counts of each chip. Nothing undocumented. The 65C02 takes one cycle more
// for JMP (abs) and one less for ASL/LSR/ROL/ROR abs,X without a page crossing; the
// functions without a Cpu are the NMOS chip's.

#include "asm/cpu.hpp"

#include <cstdint>

namespace tisple {

// mnemonics are spelled as the chip's manual spells them
enum class Mn : std::uint8_t {
    ADC,
    AND,
    ASL,
    BCC,
    BCS,
    BEQ,
    BIT,
    BMI,
    BNE,
    BPL,
    BRK,
    BVC,
    BVS,
    CLC,
    CLD,
    CLI,
    CLV,
    CMP,
    CPX,
    CPY,
    DEC,
    DEX,
    DEY,
    EOR,
    INC,
    INX,
    INY,
    JMP,
    JSR,
    LDA,
    LDX,
    LDY,
    LSR,
    NOP,
    ORA,
    PHA,
    PHP,
    PLA,
    PLP,
    ROL,
    ROR,
    RTI,
    RTS,
    SBC,
    SEC,
    SED,
    SEI,
    STA,
    STX,
    STY,
    TAX,
    TAY,
    TSX,
    TXA,
    TXS,
    TYA,
    // the 65C02's (and INC A, DEC A, BIT #/zp,X/abs,X, (zp) for the ALU group, JMP (abs,X))
    BRA,
    PHX,
    PHY,
    PLX,
    PLY,
    STZ,
    TRB,
    TSB,
    Count,
};

enum class Mode : std::uint8_t {
    Imp,  // no operand
    Acc,  // ASL A
    Imm,  // #n
    Zp,   // $nn
    ZpX,  // $nn,X (wraps inside zero page)
    ZpY,  // $nn,Y
    Abs,  // $nnnn
    AbsX, // $nnnn,X
    AbsY, // $nnnn,Y
    Ind,  // ($nnnn), JMP only
    IndX, // ($nn,X)
    IndY, // ($nn),Y
    Rel,  // branches
    // the 65C02's
    ZpInd,   // ($nn): the ALU group without Y
    AbsIndX, // ($nnnn,X), JMP only
    Count,
};

struct OpcodeInfo {
    Mn mn = Mn::BRK;
    Mode mode = Mode::Imp;
    std::uint8_t cycles = 0;
    bool page_penalty = false; // +1 when the indexed address crosses a page
    bool valid = false;
    bool cmos_only = false; // a 65C02 instruction the NMOS chip lacks
};

// -1 when the chip has no such instruction
int OpcodeFor(Mn mn, Mode mode);
int OpcodeFor(Mn mn, Mode mode, Cpu cpu);
const OpcodeInfo& OpcodeInfoFor(std::uint8_t opcode);
const OpcodeInfo& OpcodeInfoFor(std::uint8_t opcode, Cpu cpu);
const char* MnName(Mn mn);
// bytes of an instruction in this mode: 1, 2 or 3
int ModeSize(Mode mode);
bool IsBranch(Mn mn);
// the branch that tests the opposite condition (BCC <-> BCS, ...)
Mn InvertBranch(Mn mn);

} // namespace tisple
