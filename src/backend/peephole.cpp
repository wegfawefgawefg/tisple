#include "backend/peephole.hpp"

#include <map>

namespace tisple {

namespace {

constexpr int kMaxRounds = 8;
constexpr int kMaxHops = 8;

bool SameValue(const AsmValue& a, const AsmValue& b) {
    return a.sym == b.sym && a.offset == b.offset && a.part == b.part;
}

bool ReadsNZ(Mn mn) {
    return mn == Mn::BEQ || mn == Mn::BNE || mn == Mn::BMI || mn == Mn::BPL || mn == Mn::PHP;
}

bool SetsNZ(Mn mn) {
    switch (mn) {
    case Mn::LDA:
    case Mn::LDX:
    case Mn::LDY:
    case Mn::TAX:
    case Mn::TAY:
    case Mn::TXA:
    case Mn::TYA:
    case Mn::TSX:
    case Mn::PLA:
    case Mn::PLP:
    case Mn::ADC:
    case Mn::SBC:
    case Mn::AND:
    case Mn::ORA:
    case Mn::EOR:
    case Mn::CMP:
    case Mn::CPX:
    case Mn::CPY:
    case Mn::BIT:
    case Mn::INC:
    case Mn::DEC:
    case Mn::INX:
    case Mn::INY:
    case Mn::DEX:
    case Mn::DEY:
    case Mn::ASL:
    case Mn::LSR:
    case Mn::ROL:
    case Mn::ROR:
        return true;
    default:
        return false;
    }
}

bool EndsFlow(Mn mn) {
    return mn == Mn::JMP || mn == Mn::RTS || mn == Mn::RTI || mn == Mn::BRK;
}

struct Content {
    bool known = false;
    bool mem = false; // equals the memory byte at v, else the constant v
    AsmValue v;
};

bool SameContent(const Content& a, const Content& b) {
    return a.known && b.known && a.mem == b.mem && SameValue(a.v, b.v);
}

struct Pass {
    const Assembly* as = nullptr;
    std::vector<AsmItem>* items = nullptr;
    std::vector<bool> dead;
    bool changed = false;

    AsmItem& At(std::size_t i) {
        return (*items)[i];
    }

    // the first instruction or byte at or after i
    std::size_t NextReal(std::size_t i) {
        while (i < items->size() &&
               (dead[i] || At(i).kind == ItemKind::Label || At(i).kind == ItemKind::Note)) {
            i++;
        }
        return i;
    }

    // a later branch reads the flags this instruction sets
    bool FlagsNeeded(std::size_t i) {
        for (std::size_t j = i + 1; j < items->size(); j++) {
            if (dead[j] || At(j).kind == ItemKind::Note) {
                continue;
            }
            const AsmItem& item = At(j);
            if (item.kind != ItemKind::Ins) {
                return true;
            }
            if (ReadsNZ(item.mn) || IsBranch(item.mn) || EndsFlow(item.mn) || item.mn == Mn::JSR) {
                return true;
            }
            if (SetsNZ(item.mn)) {
                return false;
            }
        }
        return true;
    }

    void Kill(std::size_t i) {
        dead[i] = true;
        changed = true;
    }
};

// ---- register contents

struct Tracker {
    const Assembly* as = nullptr;
    Content reg[3];                                       // A, X, Y
    std::vector<std::pair<AsmValue, AsmValue>> mem_const; // memory byte, constant there

    void ForgetAll() {
        for (Content& c : reg) {
            c.known = false;
        }
        mem_const.clear();
    }
    void ForgetMemory() {
        for (Content& c : reg) {
            if (c.mem) {
                c.known = false;
            }
        }
        mem_const.clear();
    }
    void ForgetByte(const AsmValue& m) {
        for (Content& c : reg) {
            if (c.known && c.mem && SameValue(c.v, m)) {
                c.known = false;
            }
        }
        for (std::size_t i = 0; i < mem_const.size(); i++) {
            if (SameValue(mem_const[i].first, m)) {
                mem_const.erase(mem_const.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }
    const AsmValue* ConstAt(const AsmValue& m) const {
        for (const auto& entry : mem_const) {
            if (SameValue(entry.first, m)) {
                return &entry.second;
            }
        }
        return nullptr;
    }
};

// symbols are RAM the program owns; numbers may be I/O
bool Trackable(const Assembly& as, Mode mode, const AsmValue& v) {
    if ((mode != Mode::Abs && mode != Mode::Zp) || v.sym < 0 || v.part != Part::Whole) {
        return false;
    }
    const AsmSymbol& sym = as.symbols[static_cast<std::size_t>(v.sym)];
    return !sym.defined || sym.value < 0xC000;
}

int LoadReg(Mn mn) {
    return mn == Mn::LDA ? 0 : mn == Mn::LDX ? 1 : mn == Mn::LDY ? 2 : -1;
}

int StoreReg(Mn mn) {
    return mn == Mn::STA ? 0 : mn == Mn::STX ? 1 : mn == Mn::STY ? 2 : -1;
}

void RegisterPass(Pass& p) {
    Tracker t;
    t.as = p.as;
    for (std::size_t i = 0; i < p.items->size(); i++) {
        if (p.dead[i]) {
            continue;
        }
        AsmItem& item = p.At(i);
        if (item.kind == ItemKind::Note) {
            continue;
        }
        if (item.kind != ItemKind::Ins) {
            t.ForgetAll();
            continue;
        }
        Mn mn = item.mn;
        int r = LoadReg(mn);
        if (r >= 0) {
            Content c;
            if (item.mode == Mode::Imm) {
                c.known = true;
                c.v = item.value;
            } else if (Trackable(*p.as, item.mode, item.value)) {
                c.known = true;
                c.mem = true;
                c.v = item.value;
                if (const AsmValue* k = t.ConstAt(item.value)) {
                    c.mem = false;
                    c.v = *k;
                }
            }
            if (c.known && (SameContent(t.reg[r], c) ||
                            (c.mem && SameContent(t.reg[r], Content{true, true, item.value})))) {
                if (!p.FlagsNeeded(i)) {
                    p.Kill(i);
                    continue;
                }
            }
            // A holds it: TAX or TAY; X or Y holds it: TXA or TYA (same flags, cheaper)
            bool from_memory = item.mode != Mode::Imm && c.known;
            for (int o = 0; o < 3 && c.known; o++) {
                if (o == r || (o != 0 && r != 0)) {
                    continue;
                }
                bool same = SameContent(t.reg[o], c) ||
                            (from_memory && SameContent(t.reg[o], Content{true, true, item.value}));
                if (!same) {
                    continue;
                }
                Mn transfer = r == 1 ? Mn::TAX : r == 2 ? Mn::TAY : o == 1 ? Mn::TXA : Mn::TYA;
                item = MakeIns(transfer, Mode::Imp);
                p.changed = true;
                break;
            }
            t.reg[r] = c;
            continue;
        }
        r = StoreReg(mn);
        if (r >= 0) {
            if (!Trackable(*p.as, item.mode, item.value)) {
                t.ForgetMemory();
                continue;
            }
            t.ForgetByte(item.value);
            Content& c = t.reg[r];
            if (c.known && !c.mem) {
                t.mem_const.push_back({item.value, c.v});
            } else if (!c.known) {
                c.known = true;
                c.mem = true;
                c.v = item.value;
            }
            continue;
        }
        switch (mn) {
        case Mn::TAX:
        case Mn::TAY:
        case Mn::TXA:
        case Mn::TYA: {
            int to = mn == Mn::TAX ? 1 : mn == Mn::TAY ? 2 : 0;
            int from = mn == Mn::TXA ? 1 : mn == Mn::TYA ? 2 : 0;
            if (SameContent(t.reg[to], t.reg[from]) && !p.FlagsNeeded(i)) {
                p.Kill(i);
                continue;
            }
            t.reg[to] = t.reg[from];
            continue;
        }
        case Mn::INX:
        case Mn::DEX:
        case Mn::INY:
        case Mn::DEY: {
            Content& c = t.reg[mn == Mn::INX || mn == Mn::DEX ? 1 : 2];
            if (c.known && !c.mem && c.v.sym < 0) {
                bool up = mn == Mn::INX || mn == Mn::INY;
                c.v.offset = (c.v.offset + (up ? 1 : -1)) & 0xFF;
            } else {
                c.known = false;
            }
            continue;
        }
        case Mn::INC:
        case Mn::DEC:
        case Mn::ASL:
        case Mn::LSR:
        case Mn::ROL:
        case Mn::ROR:
            if (item.mode == Mode::Acc) {
                t.reg[0].known = false;
            } else if (Trackable(*p.as, item.mode, item.value)) {
                t.ForgetByte(item.value);
            } else {
                t.ForgetMemory();
            }
            continue;
        case Mn::ADC:
        case Mn::SBC:
        case Mn::AND:
        case Mn::ORA:
        case Mn::EOR:
        case Mn::PLA:
            t.reg[0].known = false;
            continue;
        case Mn::TSX:
            t.reg[1].known = false;
            continue;
        case Mn::JSR:
        case Mn::JMP:
        case Mn::RTS:
        case Mn::RTI:
        case Mn::BRK:
            t.ForgetAll();
            continue;
        default:
            continue;
        }
    }
}

// ---- control flow

void FlowPass(Pass& p) {
    std::map<std::int32_t, std::size_t> labels;
    for (std::size_t i = 0; i < p.items->size(); i++) {
        if (!p.dead[i] && p.At(i).kind == ItemKind::Label) {
            labels[p.At(i).value.sym] = i;
        }
    }
    // the item a label leads to; -1 if not in this function
    auto target = [&](std::int32_t label) -> std::size_t {
        auto found = labels.find(label);
        return found == labels.end() ? p.items->size() : p.NextReal(found->second);
    };
    for (std::size_t i = 0; i < p.items->size(); i++) {
        if (p.dead[i] || p.At(i).kind != ItemKind::Ins) {
            continue;
        }
        AsmItem& item = p.At(i);
        bool jump = item.mn == Mn::JMP && item.mode == Mode::Abs;
        bool branch = IsBranch(item.mn);
        if (jump || branch) {
            // follow jumps to jumps
            for (int hop = 0; hop < kMaxHops; hop++) {
                std::size_t to = target(item.value.sym);
                if (to >= p.items->size() || p.At(to).kind != ItemKind::Ins ||
                    p.At(to).mn != Mn::JMP || p.At(to).mode != Mode::Abs ||
                    p.At(to).value.sym == item.value.sym) {
                    break;
                }
                item.value = p.At(to).value;
                p.changed = true;
            }
            std::size_t to = target(item.value.sym);
            if (jump && to < p.items->size() && p.At(to).kind == ItemKind::Ins &&
                p.At(to).mn == Mn::RTS) {
                item = MakeIns(Mn::RTS, Mode::Imp);
                p.changed = true;
                continue;
            }
            // to the very next item
            if (labels.count(item.value.sym) && p.NextReal(i + 1) == to &&
                labels[item.value.sym] > i) {
                p.Kill(i);
                continue;
            }
        }
        // Bcc over a JMP: the opposite branch to where the JMP goes
        if (branch) {
            std::size_t next = p.NextReal(i + 1);
            if (next < p.items->size() && p.At(next).kind == ItemKind::Ins &&
                p.At(next).mn == Mn::JMP && p.At(next).mode == Mode::Abs &&
                target(item.value.sym) == p.NextReal(next + 1) && labels.count(item.value.sym) &&
                labels[item.value.sym] > next) {
                bool label_between = false;
                for (std::size_t j = i + 1; j < next; j++) {
                    label_between =
                        label_between || (!p.dead[j] && p.At(j).kind == ItemKind::Label);
                }
                if (!label_between) {
                    item.mn = InvertBranch(item.mn);
                    item.carry = Carry::Unknown;
                    item.value = p.At(next).value;
                    p.Kill(next);
                    continue;
                }
            }
        }
        // JSR f / RTS: JMP f
        if (item.mn == Mn::JSR) {
            std::size_t next = i + 1;
            while (next < p.items->size() && (p.dead[next] || p.At(next).kind == ItemKind::Note)) {
                next++;
            }
            if (next < p.items->size() && p.At(next).kind == ItemKind::Ins &&
                p.At(next).mn == Mn::RTS) {
                item.mn = Mn::JMP;
                p.changed = true;
            }
        }
        // nothing reaches the code after a JMP or RTS until the next label
        if (EndsFlow(item.mn)) {
            for (std::size_t j = i + 1; j < p.items->size(); j++) {
                if (p.At(j).kind == ItemKind::Label || p.At(j).kind == ItemKind::Byte) {
                    break;
                }
                if (!p.dead[j] && p.At(j).kind == ItemKind::Ins) {
                    p.Kill(j);
                }
            }
        }
    }
}

} // namespace

void Peephole(const Assembly& as, std::vector<AsmItem>& items,
              const std::vector<bool>& private_syms) {
    for (int round = 0; round < kMaxRounds; round++) {
        Pass p;
        p.as = &as;
        p.items = &items;
        p.dead.assign(items.size(), false);
        FlowPass(p);
        RegisterPass(p);
        std::vector<AsmItem> kept;
        for (std::size_t i = 0; i < items.size(); i++) {
            if (!p.dead[i]) {
                kept.push_back(items[i]);
            }
        }
        items.swap(kept);
        bool stores = DropDeadStores(items, private_syms);
        bool carry = CarryPass(items);
        bool values = DropDeadValues(as, items);
        if (!p.changed && !stores && !carry && !values) {
            return;
        }
    }
}

} // namespace tisple
