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

// an indexed byte a register may hold: v,X or v,Y with the index register as it is now, or
// (v),Y through the zero-page pointer v
enum : std::uint8_t {
    kPlain = 0,
    kByX = 1,
    kByY = 2,
    kThroughY = 3,
};

struct Content {
    bool known = false;
    bool mem = false; // equals the memory byte at v, else the constant v
    AsmValue v;
    std::uint8_t index = kPlain; // mem: how the byte at v is reached
};

bool SameContent(const Content& a, const Content& b) {
    return a.known && b.known && a.mem == b.mem && SameValue(a.v, b.v) && a.index == b.index;
}

Content PlainMem(const AsmValue& v) {
    Content c;
    c.known = true;
    c.mem = true;
    c.v = v;
    return c;
}

struct Pass {
    const Assembly* as = nullptr;
    std::vector<AsmItem>* items = nullptr;
    std::vector<bool> dead;
    std::vector<bool> flags_dead; // FlagsDeadIn, as the round began
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

    // a later branch reads the flags this instruction sets; the way on goes through labels,
    // whatever other ways into them bring
    bool FlagsNeeded(std::size_t i) {
        for (std::size_t j = i + 1; j < items->size(); j++) {
            if (dead[j] || At(j).kind == ItemKind::Note || At(j).kind == ItemKind::Label) {
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
    std::vector<std::pair<AsmValue, AsmValue>> mem_copy;  // memory byte, the byte it equals
    bool nz_a = false;                                    // N and Z show A's value
    // the flags as the last instruction left them, when it compared or loaded known values
    bool nz_known = false;
    bool c_known = false;
    bool flag_n = false;
    bool flag_z = false;
    bool flag_c = false;

    void ForgetAll() {
        for (Content& c : reg) {
            c.known = false;
        }
        mem_const.clear();
        mem_copy.clear();
        nz_a = false;
        nz_known = false;
        c_known = false;
    }
    void ForgetMemory() {
        for (Content& c : reg) {
            if (c.mem) {
                c.known = false;
            }
        }
        mem_const.clear();
        mem_copy.clear();
    }
    // what was read through index register r (1 X, 2 Y), which just changed
    void ForgetIndex(int r) {
        for (Content& c : reg) {
            bool by = r == 1 ? c.index == kByX : c.index == kByY || c.index == kThroughY;
            if (c.known && c.mem && by) {
                c.known = false;
            }
        }
    }
    void ForgetByte(const AsmValue& m) {
        for (Content& c : reg) {
            if (!c.known || !c.mem) {
                continue;
            }
            // a byte of the same array; a pointer's own bytes or what it may point at
            bool hit =
                c.index == kPlain ? SameValue(c.v, m) : c.index == kThroughY || c.v.sym == m.sym;
            if (hit) {
                c.known = false;
            }
        }
        for (std::size_t i = mem_copy.size(); i-- > 0;) {
            if (SameValue(mem_copy[i].first, m) || SameValue(mem_copy[i].second, m)) {
                mem_copy.erase(mem_copy.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        for (std::size_t i = 0; i < mem_const.size(); i++) {
            if (SameValue(mem_const[i].first, m)) {
                mem_const.erase(mem_const.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }
    // an indexed store into the array at sym (it stays inside: indices are in bounds)
    void ForgetSym(std::int32_t sym) {
        for (Content& c : reg) {
            if (c.known && c.mem && (c.v.sym == sym || c.index == kThroughY)) {
                c.known = false;
            }
        }
        auto touches = [&](const std::pair<AsmValue, AsmValue>& entry) {
            return entry.first.sym == sym || entry.second.sym == sym;
        };
        for (std::size_t i = mem_copy.size(); i-- > 0;) {
            if (touches(mem_copy[i])) {
                mem_copy.erase(mem_copy.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        for (std::size_t i = mem_const.size(); i-- > 0;) {
            if (mem_const[i].first.sym == sym) {
                mem_const.erase(mem_const.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }
    // a store that is not to a plain symbol byte
    void ForgetStore(const AsmItem& item) {
        bool indexed = item.mode == Mode::AbsX || item.mode == Mode::AbsY;
        if (indexed && item.value.sym >= 0 && item.value.part == Part::Whole) {
            ForgetSym(item.value.sym);
            return;
        }
        ForgetMemory();
    }
    // register r holds what the memory byte m holds: that byte, or one m is a copy of
    bool Holds(int r, const AsmValue& m) const {
        const Content& c = reg[r];
        if (!c.known || !c.mem || c.index != kPlain) {
            return false;
        }
        if (SameValue(c.v, m)) {
            return true;
        }
        for (const auto& copy : mem_copy) {
            if ((SameValue(copy.first, m) && SameValue(copy.second, c.v)) ||
                (SameValue(copy.second, m) && SameValue(copy.first, c.v))) {
                return true;
            }
        }
        return false;
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

bool SamePair(const std::pair<AsmValue, AsmValue>& a, const std::pair<AsmValue, AsmValue>& b) {
    return SameValue(a.first, b.first) && SameValue(a.second, b.second);
}

// what holds on both ways into a label
Tracker Meet(const Tracker& a, const Tracker& b) {
    Tracker out = a;
    for (int r = 0; r < 3; r++) {
        if (!SameContent(a.reg[r], b.reg[r])) {
            out.reg[r].known = false;
        }
    }
    auto keep = [](std::vector<std::pair<AsmValue, AsmValue>>& mine,
                   const std::vector<std::pair<AsmValue, AsmValue>>& theirs) {
        for (std::size_t i = mine.size(); i-- > 0;) {
            bool both = false;
            for (const auto& entry : theirs) {
                both = both || SamePair(mine[i], entry);
            }
            if (!both) {
                mine.erase(mine.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    };
    keep(out.mem_const, b.mem_const);
    keep(out.mem_copy, b.mem_copy);
    out.nz_a = a.nz_a && b.nz_a;
    out.nz_known = false;
    out.c_known = false;
    return out;
}

bool IsJumpTo(const AsmItem& item) {
    return item.kind == ItemKind::Ins &&
           (IsBranch(item.mn) || (item.mn == Mn::JMP && item.mode == Mode::Abs));
}

// the labels whose every way in the scan can know: only branches and JMPs name them (no jump
// table, patched operand or JMP (ind)), and they are not the function's entry, which calls
// reach. Mergeable: the jumps are all before the label, so the scan has seen them when it gets
// there. Loop: some jump comes after it, so a scan before must find out what they bring.
struct KnownLabels {
    std::vector<std::int32_t> mergeable;
    std::vector<std::int32_t> loop;
};

bool Contains(const std::vector<std::int32_t>& list, std::int32_t sym) {
    for (std::int32_t s : list) {
        if (s == sym) {
            return true;
        }
    }
    return false;
}

KnownLabels FindKnownLabels(const Pass& p) {
    const std::vector<AsmItem>& items = *p.items;
    std::map<std::int32_t, std::size_t> label_at;
    bool entry = true;
    std::vector<std::int32_t> bad;
    for (std::size_t i = 0; i < items.size(); i++) {
        const AsmItem& item = items[i];
        if (p.dead[i]) {
            continue;
        }
        if (item.kind == ItemKind::Label) {
            label_at[item.value.sym] = i;
            if (entry) {
                bad.push_back(item.value.sym);
            }
            entry = false;
        }
        if (item.kind == ItemKind::Ins && item.mn == Mn::JMP && item.mode != Mode::Abs) {
            return {};
        }
    }
    // (labels nothing names are mergeable: only the fall reaches them)
    std::vector<std::int32_t> back;
    for (std::size_t i = 0; i < items.size(); i++) {
        const AsmItem& item = items[i];
        auto found = label_at.find(item.value.sym);
        if (p.dead[i] || item.kind == ItemKind::Label || found == label_at.end()) {
            continue;
        }
        if (!IsJumpTo(item)) {
            bad.push_back(item.value.sym);
        } else if (i > found->second) {
            back.push_back(item.value.sym);
        }
    }
    KnownLabels out;
    for (const auto& label : label_at) {
        if (Contains(bad, label.first)) {
            continue;
        }
        if (Contains(back, label.first)) {
            out.loop.push_back(label.first);
        } else {
            out.mergeable.push_back(label.first);
        }
    }
    return out;
}

// symbols are RAM the program owns; numbers may be I/O
bool Trackable(const Assembly& as, Mode mode, const AsmValue& v) {
    if ((mode != Mode::Abs && mode != Mode::Zp) || v.sym < 0 || v.part != Part::Whole) {
        return false;
    }
    const AsmSymbol& sym = as.symbols[static_cast<std::size_t>(v.sym)];
    return !sym.defined || sym.value < 0xC000;
}

// an indexed byte of RAM the program owns, which nothing but stores changes: abs,X and abs,Y of
// a symbol whose page and the next are below the I/O, (zp),Y through a pointer
std::uint8_t TrackableIndex(const Assembly& as, Mode mode, const AsmValue& v) {
    if (v.sym < 0 || v.part != Part::Whole) {
        return kPlain;
    }
    const AsmSymbol& sym = as.symbols[static_cast<std::size_t>(v.sym)];
    // the offset wraps: arr-1 is arr+65535
    if (sym.defined && ((sym.value + v.offset) & 0xFFFF) + 0x100 > 0xC000) {
        return kPlain;
    }
    switch (mode) {
    case Mode::AbsX:
        return kByX;
    case Mode::AbsY:
        return kByY;
    case Mode::IndY:
        return kThroughY;
    default:
        return kPlain;
    }
}

// do N and Z show A's value after this instruction (before: they did before it)
bool NzFollowsA(const AsmItem& item, bool before) {
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
    case Mn::TAX:
    case Mn::TAY:
        return true;
    case Mn::ASL:
    case Mn::LSR:
    case Mn::ROL:
    case Mn::ROR:
        return item.mode == Mode::Acc;
    case Mn::STA:
    case Mn::STX:
    case Mn::STY:
    case Mn::CLC:
    case Mn::SEC:
    case Mn::CLV:
    case Mn::NOP:
    case Mn::PHA:
    case Mn::PHP:
    case Mn::TXS:
        return before;
    default:
        return IsBranch(item.mn) && before;
    }
}

int LoadReg(Mn mn) {
    return mn == Mn::LDA ? 0 : mn == Mn::LDX ? 1 : mn == Mn::LDY ? 2 : -1;
}

// an instruction that leaves every flag as it was
bool KeepsFlags(Mn mn) {
    switch (mn) {
    case Mn::STA:
    case Mn::STX:
    case Mn::STY:
    case Mn::NOP:
    case Mn::PHA:
    case Mn::PHP:
    case Mn::TXS:
        return true;
    default:
        return IsBranch(mn);
    }
}

// the number a register holds, if the tracker knows it
bool ConstReg(const Tracker& t, int r, int& value) {
    const Content& c = t.reg[r];
    if (!c.known || c.mem || c.v.sym >= 0) {
        return false;
    }
    value = c.v.offset & 0xFF;
    return true;
}

// the flags after the instruction, when they follow from known values (before it runs)
void KnownFlags(const Tracker& before, const AsmItem& item, Tracker& after) {
    after.nz_known = false;
    after.c_known = false;
    int operand = -1;
    if (item.mode == Mode::Imm && item.value.sym < 0) {
        operand = item.value.offset & 0xFF;
    }
    int reg = -1;
    switch (item.mn) {
    case Mn::LDA:
    case Mn::LDX:
    case Mn::LDY:
        if (operand >= 0) {
            after.nz_known = true;
            after.flag_n = (operand & 0x80) != 0;
            after.flag_z = operand == 0;
            after.c_known = before.c_known;
            after.flag_c = before.flag_c;
        }
        return;
    case Mn::CMP:
        reg = 0;
        break;
    case Mn::CPX:
        reg = 1;
        break;
    case Mn::CPY:
        reg = 2;
        break;
    default:
        return;
    }
    int value = 0;
    if (operand < 0 || !ConstReg(before, reg, value)) {
        return;
    }
    int diff = (value - operand) & 0xFF;
    after.nz_known = true;
    after.c_known = true;
    after.flag_n = (diff & 0x80) != 0;
    after.flag_z = diff == 0;
    after.flag_c = value >= operand;
}

int BranchOutcome(const Tracker& t, Mn mn);

// where a jump to `label` really goes when the registers are as t says: past a compare of a
// known register with a number and a branch it then takes, if nothing reads the flags the
// compare would have set where that branch goes. -1 when it goes to label after all.
std::int32_t ThreadTarget(Pass& p, const Tracker& t, std::int32_t label,
                          const std::map<std::int32_t, std::size_t>& labels) {
    auto found = labels.find(label);
    if (found == labels.end()) {
        return -1;
    }
    std::size_t at = p.NextReal(found->second);
    if (at >= p.items->size() || p.At(at).kind != ItemKind::Ins) {
        return -1;
    }
    const AsmItem& cmp = p.At(at);
    if (cmp.mn != Mn::CMP && cmp.mn != Mn::CPX && cmp.mn != Mn::CPY) {
        return -1;
    }
    Tracker flags = t;
    KnownFlags(t, cmp, flags);
    std::size_t next = p.NextReal(at + 1);
    if (next >= p.items->size() || p.At(next).kind != ItemKind::Ins || !IsBranch(p.At(next).mn) ||
        BranchOutcome(flags, p.At(next).mn) != 1) {
        return -1;
    }
    std::int32_t to = p.At(next).value.sym;
    auto target = labels.find(to);
    if (target == labels.end() || !p.flags_dead[target->second]) {
        return -1;
    }
    return to;
}

// a branch whose way the known flags decide: 1 taken, 0 not, -1 not known
int BranchOutcome(const Tracker& t, Mn mn) {
    switch (mn) {
    case Mn::BEQ:
        return t.nz_known ? t.flag_z : -1;
    case Mn::BNE:
        return t.nz_known ? !t.flag_z : -1;
    case Mn::BMI:
        return t.nz_known ? t.flag_n : -1;
    case Mn::BPL:
        return t.nz_known ? !t.flag_n : -1;
    case Mn::BCS:
        return t.c_known ? t.flag_c : -1;
    case Mn::BCC:
        return t.c_known ? !t.flag_c : -1;
    default:
        return -1;
    }
}

// what a load puts in its register, when the tracker can say
Content LoadContent(const Pass& p, const Tracker& t, const AsmItem& item) {
    Content c;
    std::uint8_t index = TrackableIndex(*p.as, item.mode, item.value);
    if (item.mode == Mode::Imm) {
        c.known = true;
        c.v = item.value;
    } else if (Trackable(*p.as, item.mode, item.value)) {
        c = PlainMem(item.value);
        if (const AsmValue* k = t.ConstAt(item.value)) {
            c.mem = false;
            c.v = *k;
        }
    } else if (index != kPlain) {
        c = PlainMem(item.value);
        c.index = index;
    }
    return c;
}

// A loop label that only the fall and jumps back reach, followed by a load whose value every
// jump back brings already (fill's LDA v, when nothing in the loop changes A): the load goes
// before the label, so it runs once. Returns the load's index, or 0.
std::size_t HoistableLoad(Pass& p, const Tracker& fall, const Tracker& back, std::size_t label) {
    std::size_t j = label + 1;
    while (j < p.items->size() && (p.dead[j] || p.At(j).kind == ItemKind::Note)) {
        j++;
    }
    if (j >= p.items->size() || p.At(j).kind != ItemKind::Ins) {
        return 0;
    }
    const AsmItem& load = p.At(j);
    int r = LoadReg(load.mn);
    if (r < 0) {
        return 0;
    }
    Content c = LoadContent(p, back, load);
    if (!c.known || !SameContent(back.reg[r], c) || SameContent(fall.reg[r], c) ||
        p.FlagsNeeded(j)) {
        return 0;
    }
    return j;
}

int StoreReg(Mn mn) {
    return mn == Mn::STA ? 0 : mn == Mn::STX ? 1 : mn == Mn::STY ? 2 : -1;
}

// what holds where jumps go back to a loop label, from a scan that knew nothing at the labels
using BackStates = std::map<std::int32_t, Tracker>;

// One scan, tracking what the registers hold and changing what that makes needless. With
// `back` (what a scan of the same code without it found at the jumps back to each loop label),
// a loop label holds what holds on every way in; that scan's facts at those jumps held without
// assuming anything at the labels, and every change keeps what the registers and memory hold,
// so they hold here too. With `record`, the scan notes those jumps' states instead.
void RegisterPass(Pass& p, const BackStates* back, BackStates* record) {
    Tracker t;
    t.as = p.as;
    KnownLabels known = FindKnownLabels(p);
    auto is_mergeable = [&](std::int32_t sym) { return Contains(known.mergeable, sym); };
    auto is_loop = [&](std::int32_t sym) { return Contains(known.loop, sym); };
    // what holds on the jumps seen so far into each mergeable label
    std::map<std::int32_t, Tracker> arriving;
    std::map<std::int32_t, std::size_t> labels;
    for (std::size_t i = 0; i < p.items->size(); i++) {
        if (!p.dead[i] && p.At(i).kind == ItemKind::Label) {
            labels[p.At(i).value.sym] = i;
        }
    }
    bool falls = true; // the item before may fall into this one
    for (std::size_t i = 0; i < p.items->size(); i++) {
        if (p.dead[i]) {
            continue;
        }
        AsmItem& item = p.At(i);
        if (item.kind == ItemKind::Note) {
            continue;
        }
        bool loop_label = item.kind == ItemKind::Label && is_loop(item.value.sym);
        if (item.kind == ItemKind::Label && (is_mergeable(item.value.sym) || loop_label)) {
            auto found = arriving.find(item.value.sym);
            bool any = falls;
            if (found != arriving.end()) {
                t = any ? Meet(t, found->second) : found->second;
                any = true;
            }
            if (loop_label) {
                auto from_back = back ? back->find(item.value.sym) : BackStates::const_iterator();
                std::size_t hoist =
                    back && from_back != back->end() && falls && found == arriving.end()
                        ? HoistableLoad(p, t, from_back->second, i)
                        : 0;
                if (hoist) {
                    // the load, then the label: look at the load again
                    std::swap(p.At(i), p.At(hoist));
                    labels[p.At(hoist).value.sym] = hoist;
                    p.changed = true;
                    i--;
                    continue;
                }
                if (!back || from_back == back->end()) {
                    t.ForgetAll();
                } else {
                    t = any ? Meet(t, from_back->second) : from_back->second;
                    any = true;
                }
                // a jump back may come from where the flags no longer show A
                t.nz_a = false;
            }
            if (!any) {
                t.ForgetAll();
            }
            falls = true;
            continue;
        }
        if (item.kind != ItemKind::Ins) {
            t.ForgetAll();
            falls = item.kind == ItemKind::Label;
            continue;
        }
        if (record && IsJumpTo(item) && is_loop(item.value.sym) && labels[item.value.sym] < i) {
            auto found = record->find(item.value.sym);
            if (found == record->end()) {
                record->emplace(item.value.sym, t);
            } else {
                found->second = Meet(found->second, t);
            }
        }
        // a branch the flags decide: gone, or a JMP (the flags stay for the code it reaches)
        int outcome = IsBranch(item.mn) ? BranchOutcome(t, item.mn) : -1;
        if (outcome == 0) {
            p.Kill(i);
            continue;
        }
        if (outcome == 1) {
            item = MakeIns(Mn::JMP, Mode::Abs, item.value);
            p.changed = true;
        }
        // a jump to a test the registers decide goes where the test would send it
        if (IsJumpTo(item)) {
            std::int32_t to = ThreadTarget(p, t, item.value.sym, labels);
            if (to >= 0) {
                item.value = SymValue(to);
                item.carry = Carry::Unknown;
                p.changed = true;
            }
        }
        if (IsJumpTo(item) && is_mergeable(item.value.sym)) {
            auto found = arriving.find(item.value.sym);
            if (found == arriving.end()) {
                arriving.emplace(item.value.sym, t);
            } else {
                found->second = Meet(found->second, t);
            }
        }
        Tracker flags = t;
        KnownFlags(t, item, flags);
        if (!KeepsFlags(item.mn)) {
            t.nz_known = flags.nz_known;
            t.c_known = flags.c_known;
            t.flag_n = flags.flag_n;
            t.flag_z = flags.flag_z;
            t.flag_c = flags.flag_c;
        }
        falls = !EndsFlow(item.mn);
        bool nz_before = t.nz_a;
        t.nz_a = NzFollowsA(item, nz_before);
        Mn mn = item.mn;
        int r = LoadReg(mn);
        if (r >= 0) {
            Content c = LoadContent(p, t, item);
            bool plain = c.index == kPlain;
            if (c.known &&
                (SameContent(t.reg[r], c) || (c.mem && plain && t.Holds(r, item.value)) ||
                 (c.mem && plain && SameContent(t.reg[r], PlainMem(item.value))))) {
                if (!p.FlagsNeeded(i) || (r == 0 && nz_before)) {
                    // nothing reads the flags, or they already show A
                    t.nz_a = nz_before;
                    p.Kill(i);
                    continue;
                }
                if (r == 0) {
                    // the flags for A, without loading it again
                    item = MakeIns(Mn::AND, Mode::Imm, Num(0xFF));
                    p.changed = true;
                    continue;
                }
            }
            // A holds it: TAX or TAY; X or Y holds it: TXA or TYA (same flags, cheaper)
            bool from_memory = item.mode != Mode::Imm && c.known && plain;
            for (int o = 0; o < 3 && c.known; o++) {
                if (o == r || (o != 0 && r != 0)) {
                    continue;
                }
                bool same = SameContent(t.reg[o], c) ||
                            (from_memory && SameContent(t.reg[o], PlainMem(item.value)));
                if (!same) {
                    continue;
                }
                Mn transfer = r == 1 ? Mn::TAX : r == 2 ? Mn::TAY : o == 1 ? Mn::TXA : Mn::TYA;
                item = MakeIns(transfer, Mode::Imp);
                p.changed = true;
                break;
            }
            // what was read through X or Y is gone when it changes
            if (r != 0) {
                t.ForgetIndex(r);
            }
            t.reg[r] = c;
            continue;
        }
        r = StoreReg(mn);
        if (r >= 0) {
            if (!Trackable(*p.as, item.mode, item.value)) {
                t.ForgetStore(item);
                // the register now holds what the indexed byte holds
                std::uint8_t index = TrackableIndex(*p.as, item.mode, item.value);
                if (index != kPlain && !t.reg[r].known) {
                    t.reg[r] = PlainMem(item.value);
                    t.reg[r].index = index;
                }
                continue;
            }
            // a store of what the byte already holds (the flags stay as they were)
            const Content& now = t.reg[r];
            const AsmValue* there = t.ConstAt(item.value);
            bool same = t.Holds(r, item.value) ||
                        (now.known && !now.mem && there && SameValue(*there, now.v));
            if (same) {
                p.Kill(i);
                continue;
            }
            t.ForgetByte(item.value);
            Content& c = t.reg[r];
            if (c.known && !c.mem) {
                t.mem_const.push_back({item.value, c.v});
            } else if (c.known && c.mem && c.index == kPlain) {
                t.mem_copy.push_back({item.value, c.v});
            } else {
                // a plain byte outlasts an indexed one
                c = PlainMem(item.value);
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
            if (to != 0) {
                t.ForgetIndex(to);
            }
            continue;
        }
        case Mn::INX:
        case Mn::DEX:
        case Mn::INY:
        case Mn::DEY: {
            int index = mn == Mn::INX || mn == Mn::DEX ? 1 : 2;
            Content& c = t.reg[index];
            if (c.known && !c.mem && c.v.sym < 0) {
                bool up = mn == Mn::INX || mn == Mn::INY;
                c.v.offset = (c.v.offset + (up ? 1 : -1)) & 0xFF;
            } else {
                c.known = false;
            }
            t.ForgetIndex(index);
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
                t.ForgetStore(item);
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
            t.ForgetIndex(1);
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
              const std::vector<StoreScope>& scopes) {
    for (int round = 0; round < kMaxRounds; round++) {
        Pass p;
        p.as = &as;
        p.items = &items;
        p.dead.assign(items.size(), false);
        p.flags_dead = FlagsDeadIn(as, items);
        FlowPass(p);
        // a scan of a copy finds what the jumps back to loops bring, then the real one
        std::vector<AsmItem> copy = items;
        Pass first = p;
        first.items = &copy;
        BackStates back;
        RegisterPass(first, nullptr, &back);
        RegisterPass(p, &back, nullptr);
        std::vector<AsmItem> kept;
        for (std::size_t i = 0; i < items.size(); i++) {
            if (!p.dead[i]) {
                kept.push_back(items[i]);
            }
        }
        items.swap(kept);
        bool stores = DropDeadStores(items, scopes);
        bool carry = CarryPass(items);
        bool values = DropDeadValues(as, items);
        if (!p.changed && !stores && !carry && !values) {
            return;
        }
    }
}

} // namespace tisple
