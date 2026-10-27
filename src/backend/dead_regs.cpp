#include "backend/peephole.hpp"

#include <map>

// Loads and ALU results nothing reads: liveness of A, X, Y and the N, Z, C, V flags runs
// backwards over the function's items until it settles, and an instruction whose only
// effect is registers and flags that are all dead before their next write goes. Only
// instructions without other effects can go: no stores, no read of memory that may be I/O
// (a number, a symbol at $C000 or above, anything through a pointer), no stack. Leaving the
// function (RTS, a call, a jump elsewhere) counts as reading everything.

namespace tisple {

namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

enum : std::uint8_t {
    kA = 1,
    kX = 2,
    kY = 4,
    kN = 8,
    kZ = 16,
    kC = 32,
    kV = 64,
    kAll = 127,
};

struct Effect {
    std::uint8_t uses = 0;
    std::uint8_t defs = 0;
    bool removable = false; // nothing but its registers and flags change
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

Effect EffectOf(const Assembly& as, const AsmItem& item) {
    Effect e;
    std::uint8_t index = IndexUses(item.mode);
    bool quiet = QuietRead(as, item);
    switch (item.mn) {
    case Mn::LDA:
        return Effect{index, kA | kN | kZ, quiet};
    case Mn::LDX:
        return Effect{index, kX | kN | kZ, quiet};
    case Mn::LDY:
        return Effect{index, kY | kN | kZ, quiet};
    case Mn::TAX:
        return Effect{kA, kX | kN | kZ, true};
    case Mn::TAY:
        return Effect{kA, kY | kN | kZ, true};
    case Mn::TXA:
        return Effect{kX, kA | kN | kZ, true};
    case Mn::TYA:
        return Effect{kY, kA | kN | kZ, true};
    case Mn::INX:
    case Mn::DEX:
        return Effect{kX, kX | kN | kZ, true};
    case Mn::INY:
    case Mn::DEY:
        return Effect{kY, kY | kN | kZ, true};
    case Mn::ADC:
    case Mn::SBC:
        return Effect{static_cast<std::uint8_t>(index | kA | kC), kA | kN | kZ | kC | kV, quiet};
    case Mn::AND:
    case Mn::ORA:
    case Mn::EOR:
        return Effect{static_cast<std::uint8_t>(index | kA), kA | kN | kZ, quiet};
    case Mn::CMP:
        return Effect{static_cast<std::uint8_t>(index | kA), kN | kZ | kC, quiet};
    case Mn::CPX:
        return Effect{static_cast<std::uint8_t>(index | kX), kN | kZ | kC, quiet};
    case Mn::CPY:
        return Effect{static_cast<std::uint8_t>(index | kY), kN | kZ | kC, quiet};
    case Mn::BIT:
        return Effect{kA, kN | kZ | kV, quiet};
    case Mn::CLC:
    case Mn::SEC:
        return Effect{0, kC, true};
    case Mn::CLV:
        return Effect{0, kV, true};
    case Mn::ASL:
    case Mn::LSR:
        if (item.mode == Mode::Acc) {
            return Effect{kA, kA | kN | kZ | kC, true};
        }
        return Effect{index, kN | kZ | kC, false};
    case Mn::ROL:
    case Mn::ROR:
        if (item.mode == Mode::Acc) {
            return Effect{kA | kC, kA | kN | kZ | kC, true};
        }
        return Effect{static_cast<std::uint8_t>(index | kC), kN | kZ | kC, false};
    case Mn::INC:
    case Mn::DEC:
        return Effect{index, kN | kZ, false};
    case Mn::STA:
        return Effect{static_cast<std::uint8_t>(index | kA), 0, false};
    case Mn::STX:
        return Effect{static_cast<std::uint8_t>(index | kX), 0, false};
    case Mn::STY:
        return Effect{static_cast<std::uint8_t>(index | kY), 0, false};
    case Mn::BEQ:
    case Mn::BNE:
        return Effect{kZ, 0, false};
    case Mn::BMI:
    case Mn::BPL:
        return Effect{kN, 0, false};
    case Mn::BCC:
    case Mn::BCS:
        return Effect{kC, 0, false};
    case Mn::BVC:
    case Mn::BVS:
        return Effect{kV, 0, false};
    case Mn::PHA:
        return Effect{kA, 0, false};
    case Mn::PLA:
        return Effect{0, kA | kN | kZ, false};
    case Mn::TXS:
        return Effect{kX, 0, false};
    case Mn::TSX:
        return Effect{0, kX | kN | kZ, false};
    case Mn::NOP:
        return Effect{0, 0, false};
    default:
        // calls, returns, jumps, flag and stack instructions: read everything
        e.uses = kAll;
        e.defs = 0;
        return e;
    }
}

struct Flow {
    std::map<std::int32_t, std::size_t> labels;
    std::vector<std::size_t> dispatch; // labels a jump table's bytes name
};

// where control may go after item i; kNone stands for leaving the function
void Successors(const std::vector<AsmItem>& items, const Flow& f, std::size_t i,
                std::vector<std::size_t>& out) {
    out.clear();
    const AsmItem& item = items[i];
    std::size_t next = i + 1 < items.size() ? i + 1 : kNone;
    if (item.kind == ItemKind::Byte) {
        return;
    }
    if (item.kind != ItemKind::Ins) {
        out.push_back(next);
        return;
    }
    auto target = [&](std::int32_t label) {
        auto found = f.labels.find(label);
        return found == f.labels.end() ? kNone : found->second;
    };
    switch (item.mn) {
    case Mn::JMP:
        if (item.mode == Mode::Abs) {
            out.push_back(target(item.value.sym));
            return;
        }
        for (const auto& label : f.labels) {
            out.push_back(label.second);
        }
        out.push_back(kNone);
        return;
    case Mn::RTS:
        out.insert(out.end(), f.dispatch.begin(), f.dispatch.end());
        out.push_back(kNone);
        return;
    case Mn::RTI:
    case Mn::BRK:
        out.push_back(kNone);
        return;
    case Mn::JSR:
        // the callee reads anything, then comes back here
        out.push_back(kNone);
        out.push_back(next);
        return;
    default:
        break;
    }
    out.push_back(next);
    if (IsBranch(item.mn)) {
        out.push_back(target(item.value.sym));
    }
}

// TXA or TYA, and right after it a CMP that CPX or CPY can do
bool RegisterCompare(const AsmItem& transfer, const AsmItem& cmp) {
    if (transfer.kind != ItemKind::Ins || cmp.kind != ItemKind::Ins || cmp.mn != Mn::CMP) {
        return false;
    }
    if (transfer.mn != Mn::TXA && transfer.mn != Mn::TYA) {
        return false;
    }
    return cmp.mode == Mode::Imm || cmp.mode == Mode::Zp || cmp.mode == Mode::Abs;
}

} // namespace

bool DropDeadValues(const Assembly& as, std::vector<AsmItem>& items) {
    Flow f;
    for (std::size_t i = 0; i < items.size(); i++) {
        if (items[i].kind == ItemKind::Label) {
            f.labels[items[i].value.sym] = i;
        }
    }
    for (const AsmItem& item : items) {
        if (item.kind == ItemKind::Byte && item.value.sym >= 0 && f.labels.count(item.value.sym)) {
            f.dispatch.push_back(f.labels[item.value.sym]);
        }
    }
    std::vector<Effect> effects(items.size());
    for (std::size_t i = 0; i < items.size(); i++) {
        if (items[i].kind == ItemKind::Ins) {
            effects[i] = EffectOf(as, items[i]);
        }
    }
    std::vector<std::uint8_t> live_in(items.size(), 0);
    std::vector<std::uint8_t> live_out(items.size(), 0);
    std::vector<std::size_t> succ;
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = items.size(); i-- > 0;) {
            Successors(items, f, i, succ);
            std::uint8_t out = 0;
            for (std::size_t s : succ) {
                out |= s == kNone ? static_cast<std::uint8_t>(kAll) : live_in[s];
            }
            live_out[i] = out;
            const Effect& e = effects[i];
            std::uint8_t in = out;
            if (items[i].kind == ItemKind::Ins) {
                in = static_cast<std::uint8_t>(e.uses | (out & ~e.defs));
            }
            if (in != live_in[i]) {
                live_in[i] = in;
                changed = true;
            }
        }
    }
    std::vector<AsmItem> kept;
    bool dropped = false;
    for (std::size_t i = 0; i < items.size(); i++) {
        const Effect& e = effects[i];
        bool dead = items[i].kind == ItemKind::Ins && e.removable && e.defs != 0 &&
                    (e.defs & live_out[i]) == 0;
        if (dead) {
            dropped = true;
            continue;
        }
        // TXA / CMP m with A unread after: CPX m (the compare sets every flag TXA did)
        if (i + 1 < items.size() && RegisterCompare(items[i], items[i + 1]) &&
            (live_out[i + 1] & kA) == 0) {
            AsmItem cmp = items[i + 1];
            cmp.mn = items[i].mn == Mn::TXA ? Mn::CPX : Mn::CPY;
            kept.push_back(cmp);
            dropped = true;
            i++;
            continue;
        }
        kept.push_back(items[i]);
    }
    items.swap(kept);
    return dropped;
}

} // namespace tisple
