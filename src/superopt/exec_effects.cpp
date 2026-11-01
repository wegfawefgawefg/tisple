// which instructions the superoptimizer's evaluator runs, and what each reads and writes
// (exec.hpp)

#include "superopt/exec.hpp"

namespace tisple {

bool IsModelled(Mn mn) {
    switch (mn) {
    case Mn::ADC:
    case Mn::AND:
    case Mn::ASL:
    case Mn::BIT:
    case Mn::CLC:
    case Mn::CLV:
    case Mn::CMP:
    case Mn::CPX:
    case Mn::CPY:
    case Mn::DEC:
    case Mn::DEX:
    case Mn::DEY:
    case Mn::EOR:
    case Mn::INC:
    case Mn::INX:
    case Mn::INY:
    case Mn::LDA:
    case Mn::LDX:
    case Mn::LDY:
    case Mn::LSR:
    case Mn::ORA:
    case Mn::ROL:
    case Mn::ROR:
    case Mn::SBC:
    case Mn::SEC:
    case Mn::STA:
    case Mn::STX:
    case Mn::STY:
    case Mn::TAX:
    case Mn::TAY:
    case Mn::TXA:
    case Mn::TYA:
    case Mn::STZ:
    case Mn::TSB:
    case Mn::TRB:
        return true;
    default:
        return false;
    }
}

LocEffects EffectsOf(Mn mn, Mode mode) {
    LocEffects e;
    bool memory = mode != Mode::Imp && mode != Mode::Acc && mode != Mode::Imm && mode != Mode::Rel;
    switch (mode) {
    case Mode::ZpX:
    case Mode::AbsX:
    case Mode::IndX:
        e.reads |= kLocX;
        break;
    case Mode::ZpY:
    case Mode::AbsY:
    case Mode::IndY:
        e.reads |= kLocY;
        break;
    default:
        break;
    }
    const std::uint8_t nz = kLocN | kLocZ;
    switch (mn) {
    case Mn::LDA:
        e.writes = kLocA | nz;
        break;
    case Mn::LDX:
        e.writes = kLocX | nz;
        break;
    case Mn::LDY:
        e.writes = kLocY | nz;
        break;
    case Mn::STA:
        e.reads |= kLocA;
        break;
    case Mn::STX:
        e.reads |= kLocX;
        break;
    case Mn::STY:
        e.reads |= kLocY;
        break;
    case Mn::TSB:
    case Mn::TRB:
        e.reads |= kLocA;
        e.writes = kLocZ;
        break;
    case Mn::ADC:
    case Mn::SBC:
        e.reads |= kLocA | kLocC;
        e.writes = kLocA | kLocFlags;
        break;
    case Mn::AND:
    case Mn::ORA:
    case Mn::EOR:
        e.reads |= kLocA;
        e.writes = kLocA | nz;
        break;
    case Mn::CMP:
        e.reads |= kLocA;
        e.writes = nz | kLocC;
        break;
    case Mn::CPX:
        e.reads |= kLocX;
        e.writes = nz | kLocC;
        break;
    case Mn::CPY:
        e.reads |= kLocY;
        e.writes = nz | kLocC;
        break;
    case Mn::BIT:
        e.reads |= kLocA;
        e.writes = mode == Mode::Imm ? kLocZ : nz | kLocV; // the 65C02's BIT #: only Z
        break;
    case Mn::ASL:
    case Mn::LSR:
        e.reads |= mode == Mode::Acc ? kLocA : 0;
        e.writes = (mode == Mode::Acc ? kLocA : 0) | nz | kLocC;
        break;
    case Mn::ROL:
    case Mn::ROR:
        e.reads |= (mode == Mode::Acc ? kLocA : 0) | kLocC;
        e.writes = (mode == Mode::Acc ? kLocA : 0) | nz | kLocC;
        break;
    case Mn::INC:
    case Mn::DEC:
        e.reads |= mode == Mode::Acc ? kLocA : 0; // the 65C02's INC A, DEC A
        e.writes = (mode == Mode::Acc ? kLocA : 0) | nz;
        break;
    case Mn::INX:
    case Mn::DEX:
        e.reads |= kLocX;
        e.writes = kLocX | nz;
        break;
    case Mn::INY:
    case Mn::DEY:
        e.reads |= kLocY;
        e.writes = kLocY | nz;
        break;
    case Mn::TAX:
        e.reads |= kLocA;
        e.writes = kLocX | nz;
        break;
    case Mn::TAY:
        e.reads |= kLocA;
        e.writes = kLocY | nz;
        break;
    case Mn::TXA:
        e.reads |= kLocX;
        e.writes = kLocA | nz;
        break;
    case Mn::TYA:
        e.reads |= kLocY;
        e.writes = kLocA | nz;
        break;
    case Mn::CLC:
    case Mn::SEC:
        e.writes = kLocC;
        break;
    case Mn::CLV:
        e.writes = kLocV;
        break;
    case Mn::BCC:
    case Mn::BCS:
        e.reads |= kLocC;
        break;
    case Mn::BEQ:
    case Mn::BNE:
        e.reads |= kLocZ;
        break;
    case Mn::BMI:
    case Mn::BPL:
        e.reads |= kLocN;
        break;
    case Mn::BVC:
    case Mn::BVS:
        e.reads |= kLocV;
        break;
    case Mn::PHA:
        e.reads |= kLocA;
        break;
    case Mn::PHP:
        e.reads |= kLocFlags;
        break;
    case Mn::PLA:
        e.writes = kLocA | nz;
        break;
    case Mn::PLP:
        e.writes = kLocFlags;
        break;
    case Mn::TSX:
        e.writes = kLocX | nz;
        break;
    case Mn::TXS:
        e.reads |= kLocX;
        break;
    default:
        break;
    }
    if (memory) {
        bool store = mn == Mn::STA || mn == Mn::STX || mn == Mn::STY || mn == Mn::STZ;
        bool rmw = mn == Mn::INC || mn == Mn::DEC || mn == Mn::ASL || mn == Mn::LSR ||
                   mn == Mn::ROL || mn == Mn::ROR || mn == Mn::TSB || mn == Mn::TRB;
        bool jump = mn == Mn::JMP || mn == Mn::JSR;
        e.mem_read = !store && !jump;
        e.mem_write = store || rmw;
    }
    return e;
}

} // namespace tisple
