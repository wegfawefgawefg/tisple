#include "backend/cmos.hpp"

#include "backend/peephole.hpp"

namespace tisple {

namespace {

constexpr int kA = 0;
constexpr int kX = 1;
constexpr int kY = 2;

bool IsIns(const AsmItem& item, Mn mn) {
    return item.kind == ItemKind::Ins && item.mn == mn;
}

// #n with n a number: its low byte
bool LiteralImm(const AsmItem& item, int& n) {
    if (item.kind != ItemKind::Ins || item.mode != Mode::Imm || item.value.sym >= 0 ||
        item.value.part != Part::Whole) {
        return false;
    }
    n = item.value.offset & 0xFF;
    return true;
}

// the register a load or transfer writes (kA, kX, kY), or -1
int LoadReg(Mn mn) {
    switch (mn) {
    case Mn::LDA:
        return kA;
    case Mn::LDX:
        return kX;
    case Mn::LDY:
        return kY;
    default:
        return -1;
    }
}

// the registers an instruction may change, as bits 1 << kA, 1 << kX, 1 << kY; all of them for a
// call
std::uint8_t WrittenRegs(const AsmItem& item) {
    switch (item.mn) {
    case Mn::LDA:
    case Mn::TXA:
    case Mn::TYA:
    case Mn::PLA:
    case Mn::ADC:
    case Mn::SBC:
    case Mn::AND:
    case Mn::ORA:
    case Mn::EOR:
        return 1 << kA;
    case Mn::ASL:
    case Mn::LSR:
    case Mn::ROL:
    case Mn::ROR:
    case Mn::INC:
    case Mn::DEC:
        return item.mode == Mode::Acc ? 1 << kA : 0;
    case Mn::LDX:
    case Mn::TAX:
    case Mn::TSX:
    case Mn::INX:
    case Mn::DEX:
    case Mn::PLX:
        return 1 << kX;
    case Mn::LDY:
    case Mn::TAY:
    case Mn::INY:
    case Mn::DEY:
    case Mn::PLY:
        return 1 << kY;
    case Mn::JSR:
    case Mn::BRK:
        return 7;
    default:
        return 0;
    }
}

// the register a store writes from (kA, kX, kY), or -1
int StoreReg(Mn mn) {
    return mn == Mn::STA ? kA : mn == Mn::STX ? kX : mn == Mn::STY ? kY : -1;
}

bool StzMode(Mode mode) {
    return mode == Mode::Zp || mode == Mode::Abs || mode == Mode::ZpX || mode == Mode::AbsX;
}

// stores of a register known to hold 0 become STZ, and (zp),Y with Y known 0 becomes (zp)
bool UseKnownZeros(std::vector<AsmItem>& items) {
    bool zero[3] = {false, false, false};
    bool changed = false;
    for (AsmItem& item : items) {
        if (item.kind == ItemKind::Label || item.kind == ItemKind::Byte) {
            zero[kA] = zero[kX] = zero[kY] = false;
            continue;
        }
        if (item.kind != ItemKind::Ins) {
            continue; // notes; a wait keeps the registers
        }
        int stored = StoreReg(item.mn);
        if (stored >= 0 && zero[stored] && StzMode(item.mode)) {
            item.mn = Mn::STZ;
            changed = true;
        }
        if (item.mode == Mode::IndY && zero[kY]) {
            item.mode = Mode::ZpInd;
            changed = true;
        }
        int loaded = LoadReg(item.mn);
        int n = -1;
        if (loaded >= 0) {
            zero[loaded] = LiteralImm(item, n) && n == 0;
            continue;
        }
        switch (item.mn) {
        case Mn::TAX:
            zero[kX] = zero[kA];
            continue;
        case Mn::TAY:
            zero[kY] = zero[kA];
            continue;
        case Mn::TXA:
            zero[kA] = zero[kX];
            continue;
        case Mn::TYA:
            zero[kA] = zero[kY];
            continue;
        default:
            break;
        }
        std::uint8_t written = WrittenRegs(item);
        for (int r = 0; r < 3; r++) {
            zero[r] = zero[r] && (written & (1 << r)) == 0;
        }
    }
    return changed;
}

// the item after i, past source notes; items.size() when none
std::size_t NextIns(const std::vector<AsmItem>& items, std::size_t i) {
    std::size_t next = i + 1;
    while (next < items.size() && items[next].kind == ItemKind::Note) {
        next++;
    }
    return next;
}

constexpr std::uint8_t kLiveAll = 0x7F;

// what is read after item i (falling through to the next item)
std::uint8_t LiveAfter(const std::vector<AsmItem>& items, const std::vector<std::uint8_t>& live,
                       std::size_t i) {
    std::size_t next = i + 1;
    return next < items.size() ? live[next] : kLiveAll;
}

// a byte of RAM the program owns at a fixed place: a symbol below the I/O page
bool OwnRam(const Assembly& as, const AsmItem& item) {
    if (item.value.sym < 0 || item.value.part != Part::Whole) {
        return false;
    }
    const AsmSymbol& sym = as.symbols[static_cast<std::size_t>(item.value.sym)];
    return !sym.defined || sym.value + 0x100 < 0xC000;
}

bool SameOperand(const AsmItem& a, const AsmItem& b) {
    return a.mode == b.mode && a.value.sym == b.value.sym && a.value.offset == b.value.offset &&
           a.value.part == b.value.part;
}

// CLC/ADC #1 (#$FF) or SEC/SBC #1 (#$FF) at i and j: INC A or DEC A, or none (Count)
Mn StepOfA(const AsmItem& flag, const AsmItem& op) {
    int n = -1;
    if (!LiteralImm(op, n) || (n != 1 && n != 0xFF)) {
        return Mn::Count;
    }
    bool up = n == 1;
    if (IsIns(flag, Mn::CLC) && IsIns(op, Mn::ADC)) {
        return up ? Mn::INC : Mn::DEC;
    }
    if (IsIns(flag, Mn::SEC) && IsIns(op, Mn::SBC)) {
        return up ? Mn::DEC : Mn::INC;
    }
    return Mn::Count;
}

// TXA/PHA -> PHX and the others: the pair at i, j becomes one instruction, or none (Count)
Mn RegisterStack(const AsmItem& first, const AsmItem& second, std::uint8_t after) {
    bool a_dead = (after & kLiveA) == 0;
    bool nz_dead = (after & (kLiveN | kLiveZ)) == 0;
    if (IsIns(second, Mn::PHA) && a_dead && nz_dead) {
        return IsIns(first, Mn::TXA) ? Mn::PHX : IsIns(first, Mn::TYA) ? Mn::PHY : Mn::Count;
    }
    if (IsIns(first, Mn::PLA) && a_dead) {
        return IsIns(second, Mn::TAX) ? Mn::PLX : IsIns(second, Mn::TAY) ? Mn::PLY : Mn::Count;
    }
    return Mn::Count;
}

// LDA m / ORA #k / STA m (AND: TRB) at i, j, l: LDA #k / TSB m, or nothing (empty)
std::vector<AsmItem> BitSetOrClear(const Assembly& as, const AsmItem& load, const AsmItem& op,
                                   const AsmItem& store) {
    bool direct = load.mode == Mode::Zp || load.mode == Mode::Abs;
    if (!IsIns(load, Mn::LDA) || !IsIns(store, Mn::STA) || !direct || !SameOperand(load, store) ||
        !OwnRam(as, load) || op.kind != ItemKind::Ins || op.mode != Mode::Imm) {
        return {};
    }
    if (op.mn == Mn::ORA) {
        return {MakeIns(Mn::LDA, Mode::Imm, op.value), MakeIns(Mn::TSB, load.mode, load.value)};
    }
    int n = -1;
    if (op.mn == Mn::AND && LiteralImm(op, n)) {
        return {MakeIns(Mn::LDA, Mode::Imm, Num(~n & 0xFF)),
                MakeIns(Mn::TRB, load.mode, load.value)};
    }
    return {};
}

// the source notes between i and end, then the new instructions
void Replace(const std::vector<AsmItem>& items, std::size_t i, std::size_t end,
             const std::vector<AsmItem>& with, std::vector<AsmItem>& out) {
    for (std::size_t k = i + 1; k < end; k++) {
        if (items[k].kind == ItemKind::Note) {
            out.push_back(items[k]);
        }
    }
    out.insert(out.end(), with.begin(), with.end());
}

// ADC #1 (#$FF) with the carry known clear, SBC #1 (#$FF) with it known set: INC A or DEC A,
// or none (Count)
Mn StepWithCarry(const AsmItem& op, std::int8_t carry) {
    int n = -1;
    if (!LiteralImm(op, n) || (n != 1 && n != 0xFF)) {
        return Mn::Count;
    }
    if (IsIns(op, Mn::ADC) && carry == 0) {
        return n == 1 ? Mn::INC : Mn::DEC;
    }
    if (IsIns(op, Mn::SBC) && carry == 1) {
        return n == 1 ? Mn::DEC : Mn::INC;
    }
    return Mn::Count;
}

// the rewrites of two and three instructions in a row, by the liveness of the code before
// and the carry it leaves
bool UsePairs(const Assembly& as, std::vector<AsmItem>& items,
              const std::set<std::int32_t>* entries) {
    std::vector<std::uint8_t> live = LiveRegsIn(as, items);
    std::vector<std::int8_t> carry = CarryKnownIn(items, entries);
    std::vector<AsmItem> out;
    out.reserve(items.size());
    bool changed = false;
    for (std::size_t i = 0; i < items.size(); i++) {
        std::size_t j = NextIns(items, i);
        Mn alone = StepWithCarry(items[i], carry[i]);
        if (alone != Mn::Count && (LiveAfter(items, live, i) & (kLiveC | kLiveV)) == 0) {
            out.push_back(MakeIns(alone, Mode::Acc));
            changed = true;
            continue;
        }
        if (items[i].kind != ItemKind::Ins || j >= items.size()) {
            out.push_back(items[i]);
            continue;
        }
        std::uint8_t after_j = LiveAfter(items, live, j);
        Mn step = StepOfA(items[i], items[j]);
        if (step != Mn::Count && (after_j & (kLiveC | kLiveV)) == 0) {
            Replace(items, i, j, {MakeIns(step, Mode::Acc)}, out);
            i = j;
            changed = true;
            continue;
        }
        Mn stack = RegisterStack(items[i], items[j], after_j);
        if (stack != Mn::Count) {
            Replace(items, i, j, {MakeIns(stack, Mode::Imp)}, out);
            i = j;
            changed = true;
            continue;
        }
        std::size_t l = NextIns(items, j);
        if (l < items.size() && (LiveAfter(items, live, l) & (kLiveA | kLiveN | kLiveZ)) == 0) {
            std::vector<AsmItem> bits = BitSetOrClear(as, items[i], items[j], items[l]);
            if (!bits.empty()) {
                Replace(items, i, l, bits, out);
                i = l;
                changed = true;
                continue;
            }
        }
        out.push_back(items[i]);
    }
    items.swap(out);
    return changed;
}

} // namespace

bool UseCmos(const Assembly& as, std::vector<AsmItem>& items,
             const std::set<std::int32_t>* entries) {
    bool changed = UsePairs(as, items, entries);
    changed = UseKnownZeros(items) || changed;
    // a CLC or SEC an INC A made redundant (in a function's code: CarryPass knows nothing of
    // other entries), then the loads of 0 and the LDY #0 nothing reads any more
    while (entries == nullptr && CarryPass(items)) {
        changed = true;
    }
    while (DropDeadValues(as, items)) {
        changed = true;
    }
    return changed;
}

} // namespace tisple
