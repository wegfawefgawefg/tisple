#include "backend/peephole.hpp"

#include <map>
#include <set>

// The carry flag along one function's control flow. A CLC or SEC that finds the carry
// already so goes; a CLC that only the way into a loop needs (the loop's own back edge
// brings the carry clear) moves before the loop's top.
//
// Facts come from the instructions (CLC, SEC, ADC and the rest, the two edges of BCC and
// BCS) and from code generation (AsmItem::carry: an add that cannot carry, the back edge of
// a counted loop). The function's start, and any label something other than a branch or a
// JMP names (jump tables, patched operands), may be entered with any carry.

namespace tisple {

namespace {

enum class CarryState : std::uint8_t {
    None, // no way here found yet
    Clear,
    Set,
    Any,
};

CarryState Meet(CarryState a, CarryState b) {
    if (a == CarryState::None) {
        return b;
    }
    if (b == CarryState::None || a == b) {
        return a;
    }
    return CarryState::Any;
}

CarryState FromFact(Carry fact) {
    return fact == Carry::Clear ? CarryState::Clear : CarryState::Set;
}

bool WritesCarry(Mn mn) {
    switch (mn) {
    case Mn::ADC:
    case Mn::SBC:
    case Mn::CMP:
    case Mn::CPX:
    case Mn::CPY:
    case Mn::ASL:
    case Mn::LSR:
    case Mn::ROL:
    case Mn::ROR:
    case Mn::CLC:
    case Mn::SEC:
    case Mn::PLP:
    case Mn::RTI:
    case Mn::BRK:
    case Mn::JSR:
        return true;
    default:
        return false;
    }
}

bool ReadsCarry(Mn mn) {
    return mn == Mn::ADC || mn == Mn::SBC || mn == Mn::ROL || mn == Mn::ROR || mn == Mn::BCC ||
           mn == Mn::BCS || mn == Mn::PHP;
}

bool EndsFlow(const AsmItem& item) {
    return item.mn == Mn::JMP || item.mn == Mn::RTS || item.mn == Mn::RTI || item.mn == Mn::BRK;
}

// the carry after a straight-line instruction
CarryState After(const AsmItem& item, CarryState in) {
    if (item.mn == Mn::CLC) {
        return CarryState::Clear;
    }
    if (item.mn == Mn::SEC) {
        return CarryState::Set;
    }
    if (!WritesCarry(item.mn)) {
        return in;
    }
    if (item.carry != Carry::Unknown) {
        return FromFact(item.carry);
    }
    return CarryState::Any;
}

CarryState Taken(const AsmItem& item, CarryState in) {
    if (item.mn == Mn::BCC) {
        return CarryState::Clear;
    }
    if (item.mn == Mn::BCS) {
        return CarryState::Set;
    }
    if (item.carry != Carry::Unknown) {
        return FromFact(item.carry);
    }
    return in;
}

CarryState FallThrough(const AsmItem& item, CarryState in) {
    if (item.mn == Mn::BCC) {
        return CarryState::Set;
    }
    if (item.mn == Mn::BCS) {
        return CarryState::Clear;
    }
    return in;
}

bool IsFlowJump(const AsmItem& item) {
    return item.kind == ItemKind::Ins &&
           (IsBranch(item.mn) || (item.mn == Mn::JMP && item.mode == Mode::Abs));
}

struct CarryFlow {
    const std::vector<AsmItem>* items = nullptr;
    std::set<std::int32_t> labels;            // in this function
    std::set<std::int32_t> unknown;           // labels entered from elsewhere
    std::map<std::int32_t, CarryState> edges; // label: the meet of the jumps to it
    std::vector<CarryState> in;               // by item: the carry on the way in
    std::vector<CarryState> fall;             // by label item: what falls into it
};

void FindLabels(CarryFlow& f) {
    const std::vector<AsmItem>& items = *f.items;
    for (const AsmItem& item : items) {
        if (item.kind == ItemKind::Label) {
            f.labels.insert(item.value.sym);
        }
    }
    for (const AsmItem& item : items) {
        if (item.kind == ItemKind::Label || item.value.sym < 0 || IsFlowJump(item)) {
            continue;
        }
        if (f.labels.count(item.value.sym)) {
            f.unknown.insert(item.value.sym);
        }
    }
    for (const AsmItem& item : items) {
        if (item.kind == ItemKind::Label) {
            f.unknown.insert(item.value.sym); // the function's start
            break;
        }
    }
}

// one pass forward; true if what some jump brings to a label changed
bool Sweep(CarryFlow& f) {
    const std::vector<AsmItem>& items = *f.items;
    bool changed = false;
    CarryState state = CarryState::Any;
    for (std::size_t i = 0; i < items.size(); i++) {
        const AsmItem& item = items[i];
        if (item.kind == ItemKind::Note) {
            f.in[i] = state;
            continue;
        }
        if (item.kind == ItemKind::Label) {
            f.fall[i] = state;
            state = Meet(state, f.edges[item.value.sym]);
            if (f.unknown.count(item.value.sym)) {
                state = CarryState::Any;
            }
            f.in[i] = state;
            continue;
        }
        f.in[i] = state;
        if (item.kind == ItemKind::Byte) {
            state = CarryState::None;
            continue;
        }
        if (IsFlowJump(item)) {
            std::int32_t target = item.value.sym;
            if (f.labels.count(target)) {
                CarryState before = f.edges[target];
                CarryState after = Meet(before, Taken(item, state));
                f.edges[target] = after;
                changed = changed || after != before;
            }
            state = item.mn == Mn::JMP ? CarryState::None : FallThrough(item, state);
            continue;
        }
        if (EndsFlow(item)) {
            state = CarryState::None;
            continue;
        }
        state = After(item, state);
    }
    return changed;
}

constexpr int kMaxSweeps = 16;

// the labels just before item i, with only instructions that leave the carry alone between;
// false if there are none or something else is in the way
bool LabelsBefore(const CarryFlow& f, std::size_t i, std::size_t& first, std::size_t& last) {
    const std::vector<AsmItem>& items = *f.items;
    std::size_t j = i;
    while (j > 0) {
        j--;
        const AsmItem& item = items[j];
        if (item.kind == ItemKind::Note) {
            continue;
        }
        if (item.kind == ItemKind::Label) {
            break;
        }
        if (item.kind != ItemKind::Ins || IsFlowJump(item) || EndsFlow(item) ||
            WritesCarry(item.mn) || ReadsCarry(item.mn)) {
            return false;
        }
        if (j == 0) {
            return false;
        }
    }
    if (items[j].kind != ItemKind::Label) {
        return false;
    }
    last = j;
    first = j;
    while (first > 0 &&
           (items[first - 1].kind == ItemKind::Label || items[first - 1].kind == ItemKind::Note)) {
        first--;
    }
    while (items[first].kind == ItemKind::Note) {
        first++;
    }
    return true;
}

// a CLC whose every way in but falling into the labels before it brings the carry clear:
// it can go there instead. Returns where to put it, or the item count for nowhere.
std::size_t HoistPlace(const CarryFlow& f, std::size_t i) {
    const std::vector<AsmItem>& items = *f.items;
    std::size_t first = 0;
    std::size_t last = 0;
    if (!LabelsBefore(f, i, first, last)) {
        return items.size();
    }
    for (std::size_t j = first; j <= last; j++) {
        if (items[j].kind != ItemKind::Label) {
            continue;
        }
        std::int32_t label = items[j].value.sym;
        if (f.unknown.count(label)) {
            return items.size();
        }
        auto found = f.edges.find(label);
        CarryState edge = found == f.edges.end() ? CarryState::None : found->second;
        if (edge != CarryState::None && edge != CarryState::Clear) {
            return items.size();
        }
    }
    if (f.fall[first] == CarryState::None || f.fall[first] == CarryState::Clear) {
        return items.size();
    }
    return first;
}

} // namespace

bool CarryPass(std::vector<AsmItem>& items) {
    CarryFlow f;
    f.items = &items;
    f.in.assign(items.size(), CarryState::None);
    f.fall.assign(items.size(), CarryState::None);
    FindLabels(f);
    for (int sweep = 0; sweep < kMaxSweeps; sweep++) {
        if (!Sweep(f)) {
            break;
        }
        if (sweep + 1 == kMaxSweeps) {
            return false; // not settled: change nothing
        }
    }
    std::vector<bool> drop(items.size(), false);
    std::vector<bool> clc_before(items.size(), false);
    bool changed = false;
    for (std::size_t i = 0; i < items.size(); i++) {
        const AsmItem& item = items[i];
        if (item.kind != ItemKind::Ins) {
            continue;
        }
        bool known = (item.mn == Mn::CLC && f.in[i] == CarryState::Clear) ||
                     (item.mn == Mn::SEC && f.in[i] == CarryState::Set);
        if (known) {
            drop[i] = true;
            changed = true;
            continue;
        }
        if (item.mn != Mn::CLC) {
            continue;
        }
        std::size_t at = HoistPlace(f, i);
        if (at < items.size() && !clc_before[at]) {
            clc_before[at] = true;
            drop[i] = true;
            changed = true;
        }
    }
    if (!changed) {
        return false;
    }
    std::vector<AsmItem> kept;
    for (std::size_t i = 0; i < items.size(); i++) {
        if (clc_before[i]) {
            kept.push_back(MakeIns(Mn::CLC, Mode::Imp));
        }
        if (!drop[i]) {
            kept.push_back(items[i]);
        }
    }
    items.swap(kept);
    return true;
}

} // namespace tisple
