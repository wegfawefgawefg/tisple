#include "backend/reg_effects.hpp"

namespace tisple {

namespace {

// (kLiveA and the others in peephole.hpp)
enum : std::uint8_t {
    kA = kLiveA,
    kX = kLiveX,
    kY = kLiveY,
    kN = kLiveN,
    kZ = kLiveZ,
    kC = kLiveC,
    kV = kLiveV,
    kAll = 127,
};

std::uint8_t IndexUses(Mode mode) {
    switch (mode) {
    case Mode::ZpX:
    case Mode::AbsX:
    case Mode::IndX:
        return kX;
    case Mode::ZpY:
    case Mode::AbsY:
    case Mode::IndY:
        return kY;
    default:
        return 0;
    }
}

// reading the operand has no effect: a number, or RAM the program owns
bool QuietRead(const Assembly& as, const AsmItem& item) {
    if (item.mode == Mode::Imm || item.mode == Mode::Acc || item.mode == Mode::Imp) {
        return true;
    }
    bool direct = item.mode == Mode::Abs || item.mode == Mode::Zp || item.mode == Mode::AbsX ||
                  item.mode == Mode::AbsY || item.mode == Mode::ZpX || item.mode == Mode::ZpY;
    if (!direct || item.value.sym < 0 || item.value.part != Part::Whole) {
        return false;
    }
    const AsmSymbol& sym = as.symbols[static_cast<std::size_t>(item.value.sym)];
    return !sym.defined || sym.value + 0x100 < 0xC000;
}

} // namespace

RegEffect EffectOf(const Assembly& as, const AsmItem& item) {
    RegEffect e;
    std::uint8_t index = IndexUses(item.mode);
    bool quiet = QuietRead(as, item);
    switch (item.mn) {
    case Mn::LDA:
        return RegEffect{index, kA | kN | kZ, quiet};
    case Mn::LDX:
        return RegEffect{index, kX | kN | kZ, quiet};
    case Mn::LDY:
        return RegEffect{index, kY | kN | kZ, quiet};
    case Mn::TAX:
        return RegEffect{kA, kX | kN | kZ, true};
    case Mn::TAY:
        return RegEffect{kA, kY | kN | kZ, true};
    case Mn::TXA:
        return RegEffect{kX, kA | kN | kZ, true};
    case Mn::TYA:
        return RegEffect{kY, kA | kN | kZ, true};
    case Mn::INX:
    case Mn::DEX:
        return RegEffect{kX, kX | kN | kZ, true};
    case Mn::INY:
    case Mn::DEY:
        return RegEffect{kY, kY | kN | kZ, true};
    case Mn::ADC:
    case Mn::SBC:
        return RegEffect{static_cast<std::uint8_t>(index | kA | kC), kA | kN | kZ | kC | kV, quiet};
    case Mn::AND:
    case Mn::ORA:
    case Mn::EOR:
        return RegEffect{static_cast<std::uint8_t>(index | kA), kA | kN | kZ, quiet};
    case Mn::CMP:
        return RegEffect{static_cast<std::uint8_t>(index | kA), kN | kZ | kC, quiet};
    case Mn::CPX:
        return RegEffect{static_cast<std::uint8_t>(index | kX), kN | kZ | kC, quiet};
    case Mn::CPY:
        return RegEffect{static_cast<std::uint8_t>(index | kY), kN | kZ | kC, quiet};
    case Mn::BIT:
        if (item.mode == Mode::Imm) { // the 65C02's BIT #: only Z
            return RegEffect{kA, kZ, true};
        }
        return RegEffect{static_cast<std::uint8_t>(index | kA), kN | kZ | kV, quiet};
    case Mn::CLC:
    case Mn::SEC:
        return RegEffect{0, kC, true};
    case Mn::CLV:
        return RegEffect{0, kV, true};
    case Mn::ASL:
    case Mn::LSR:
        if (item.mode == Mode::Acc) {
            return RegEffect{kA, kA | kN | kZ | kC, true};
        }
        return RegEffect{index, kN | kZ | kC, false};
    case Mn::ROL:
    case Mn::ROR:
        if (item.mode == Mode::Acc) {
            return RegEffect{kA | kC, kA | kN | kZ | kC, true};
        }
        return RegEffect{static_cast<std::uint8_t>(index | kC), kN | kZ | kC, false};
    case Mn::INC:
    case Mn::DEC:
        if (item.mode == Mode::Acc) { // the 65C02's INC A, DEC A
            return RegEffect{kA, kA | kN | kZ, true};
        }
        return RegEffect{index, kN | kZ, false};
    case Mn::STA:
        return RegEffect{static_cast<std::uint8_t>(index | kA), 0, false};
    case Mn::STX:
        return RegEffect{static_cast<std::uint8_t>(index | kX), 0, false};
    case Mn::STY:
        return RegEffect{static_cast<std::uint8_t>(index | kY), 0, false};
    // the 65C02's
    case Mn::STZ:
        return RegEffect{index, 0, false};
    case Mn::TSB:
    case Mn::TRB:
        return RegEffect{kA, kZ, false};
    case Mn::PHX:
        return RegEffect{kX, 0, false};
    case Mn::PHY:
        return RegEffect{kY, 0, false};
    case Mn::PLX:
        return RegEffect{0, kX | kN | kZ, false};
    case Mn::PLY:
        return RegEffect{0, kY | kN | kZ, false};
    case Mn::BEQ:
    case Mn::BNE:
        return RegEffect{kZ, 0, false};
    case Mn::BMI:
    case Mn::BPL:
        return RegEffect{kN, 0, false};
    case Mn::BCC:
    case Mn::BCS:
        return RegEffect{kC, 0, false};
    case Mn::BVC:
    case Mn::BVS:
        return RegEffect{kV, 0, false};
    case Mn::PHA:
        return RegEffect{kA, 0, false};
    case Mn::PLA:
        return RegEffect{0, kA | kN | kZ, false};
    case Mn::TXS:
        return RegEffect{kX, 0, false};
    case Mn::TSX:
        return RegEffect{0, kX | kN | kZ, false};
    case Mn::NOP:
        return RegEffect{0, 0, false};
    case Mn::JMP:
        // to a label here: what is read there (Successors); elsewhere, or through a pointer,
        // a way out, which reads everything
        if (item.mode == Mode::Abs) {
            return RegEffect{0, 0, false};
        }
        e.uses = kAll;
        e.defs = 0;
        return e;
    case Mn::JSR:
        // our own code (a symbol: a function or a runtime routine) never reads the flags it
        // is called with and leaves its own; the registers may be read, or kept
        if (item.value.sym >= 0) {
            return RegEffect{kA | kX | kY, kN | kZ | kC | kV, false};
        }
        e.uses = kAll;
        e.defs = 0;
        return e;
    default:
        // calls, returns, jumps, flag and stack instructions: read everything
        e.uses = kAll;
        e.defs = 0;
        return e;
    }
}

} // namespace tisple
