#include "asm/opcodes.hpp"
#include "backend/peephole.hpp"

#include <map>

// Stores that nothing reads before the byte is written again. Liveness runs backwards over
// the items until it settles. A private byte is dead when the function returns; an exposed
// one (a global, another function's parameter) is read by every call, jump out of the
// function and return. A byte read by an indexed or indirect mode, or whose address is taken
// (#<sym), counts as always live. Pointers never reach exposed bytes (provenance: they are
// not address-taken), so a load through one reads none of them.

namespace tisple {

namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

bool IsStore(Mn mn) {
    return mn == Mn::STA || mn == Mn::STX || mn == Mn::STY;
}

bool IsPlain(const AsmItem& item) {
    return (item.mode == Mode::Abs || item.mode == Mode::Zp) && item.value.part == Part::Whole;
}

// a set of numbered bytes
struct Bits {
    std::vector<std::uint64_t> words;

    bool Has(std::size_t i) const {
        return (words[i / 64] >> (i % 64)) & 1;
    }
    void Set(std::size_t i) {
        words[i / 64] |= std::uint64_t(1) << (i % 64);
    }
    void Clear(std::size_t i) {
        words[i / 64] &= ~(std::uint64_t(1) << (i % 64));
    }
    // this |= other; true if anything new came in
    bool Merge(const Bits& other) {
        bool grew = false;
        for (std::size_t w = 0; w < words.size(); w++) {
            std::uint64_t both = words[w] | other.words[w];
            grew = grew || both != words[w];
            words[w] = both;
        }
        return grew;
    }
};

struct Flow {
    const std::vector<AsmItem>* items = nullptr;
    std::map<std::int32_t, std::size_t> labels; // label symbol -> its item
    std::vector<std::size_t> dispatch;          // labels a jump table's bytes name
};

// where control may go after item i; kNone stands for leaving the function
void Successors(const Flow& f, std::size_t i, std::vector<std::size_t>& out) {
    out.clear();
    const AsmItem& item = (*f.items)[i];
    std::size_t next = i + 1 < f.items->size() ? i + 1 : kNone;
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
        // a match pushes a table entry and returns into it
        out.insert(out.end(), f.dispatch.begin(), f.dispatch.end());
        out.push_back(kNone);
        return;
    case Mn::RTI:
    case Mn::BRK:
        out.push_back(kNone);
        return;
    default:
        break;
    }
    out.push_back(next);
    if (IsBranch(item.mn)) {
        out.push_back(target(item.value.sym));
    }
}

// a variable's bytes, as the dead-store pass may see them from this function's code: its own
// locals and parameters and every function's locals are read only by code that writes them
// first (a callee's locals by its inlined copies, or by itself); globals and other
// functions' parameters by any code after a call or the return
StoreScope VarScope(const Var& var, std::int32_t function) {
    if (var.address_taken || var.placement.has_address || var.irq_shared) {
        return StoreScope::Untracked;
    }
    if (var.kind == VarKind::Local || (var.kind == VarKind::Param && var.function == function)) {
        return StoreScope::Private;
    }
    if (var.kind == VarKind::Global || var.kind == VarKind::Param) {
        return StoreScope::Exposed;
    }
    return StoreScope::Untracked;
}

} // namespace

bool DropDeadStores(std::vector<AsmItem>& items, const std::vector<StoreScope>& scopes) {
    // tracked bytes read one at a time get numbers; a symbol read any other way is left alone
    auto tracked = [&](std::int32_t sym) {
        return sym >= 0 && static_cast<std::size_t>(sym) < scopes.size() &&
               scopes[static_cast<std::size_t>(sym)] != StoreScope::Untracked;
    };
    std::vector<bool> whole(scopes.size(), false);
    Flow f;
    f.items = &items;
    for (std::size_t i = 0; i < items.size(); i++) {
        const AsmItem& item = items[i];
        if (item.kind == ItemKind::Label) {
            f.labels[item.value.sym] = i;
        }
        if (!tracked(item.value.sym)) {
            continue;
        }
        if (item.kind != ItemKind::Ins || !IsPlain(item)) {
            whole[static_cast<std::size_t>(item.value.sym)] = true;
        }
    }
    for (const AsmItem& item : items) {
        if (item.kind == ItemKind::Byte && item.value.sym >= 0 && f.labels.count(item.value.sym)) {
            f.dispatch.push_back(f.labels[item.value.sym]);
        }
    }
    std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> numbers;
    std::vector<bool> exposed_numbers;
    std::vector<std::size_t> byte_of(items.size(), kNone);
    for (std::size_t i = 0; i < items.size(); i++) {
        const AsmItem& item = items[i];
        if (item.kind != ItemKind::Ins || !tracked(item.value.sym) ||
            whole[static_cast<std::size_t>(item.value.sym)]) {
            continue;
        }
        auto key = std::make_pair(item.value.sym, item.value.offset);
        auto found = numbers.find(key);
        if (found == numbers.end()) {
            found = numbers.emplace(key, numbers.size()).first;
            exposed_numbers.push_back(scopes[static_cast<std::size_t>(item.value.sym)] ==
                                      StoreScope::Exposed);
        }
        byte_of[i] = found->second;
    }
    if (numbers.empty()) {
        return false;
    }
    std::size_t words = (numbers.size() + 63) / 64;
    Bits empty;
    empty.words.assign(words, 0);
    // what a call, a jump out and the return read
    Bits exposed = empty;
    for (std::size_t n = 0; n < exposed_numbers.size(); n++) {
        if (exposed_numbers[n]) {
            exposed.Set(n);
        }
    }
    std::vector<Bits> live_in(items.size(), empty);
    std::vector<Bits> live_out(items.size(), empty);
    std::vector<std::size_t> succ;
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = items.size(); i-- > 0;) {
            Successors(f, i, succ);
            for (std::size_t s : succ) {
                if (s != kNone) {
                    live_out[i].Merge(live_in[s]);
                } else {
                    live_out[i].Merge(exposed);
                }
            }
            Bits in = live_out[i];
            if (items[i].kind == ItemKind::Ins && items[i].mn == Mn::JSR) {
                in.Merge(exposed);
            }
            std::size_t byte = byte_of[i];
            if (byte != kNone) {
                if (IsStore(items[i].mn)) {
                    in.Clear(byte);
                } else {
                    in.Set(byte);
                }
            }
            changed = live_in[i].Merge(in) || changed;
        }
    }
    std::vector<AsmItem> kept;
    bool dropped = false;
    for (std::size_t i = 0; i < items.size(); i++) {
        std::size_t byte = byte_of[i];
        if (byte != kNone && IsStore(items[i].mn) && !live_out[i].Has(byte)) {
            dropped = true;
            continue;
        }
        kept.push_back(items[i]);
    }
    items.swap(kept);
    return dropped;
}

// by symbol, for this function's code
std::vector<StoreScope> StoreScopes(const Backend& be, std::int32_t function) {
    std::vector<StoreScope> out(be.info.size(), StoreScope::Untracked);
    for (std::size_t s = 0; s < be.info.size(); s++) {
        const SymInfo& info = be.info[s];
        if (info.role == SymRole::Var) {
            out[s] = VarScope(be.program->vars[static_cast<std::size_t>(info.owner)], function);
            continue;
        }
        bool frame = info.role == SymRole::Temps || info.role == SymRole::PtrTemps ||
                     info.role == SymRole::LoopPtrs;
        if (frame && info.owner == function) {
            out[s] = StoreScope::Private;
        }
    }
    return out;
}

// by symbol, for this function's code: the function whose frame holds the byte, for the
// locals and parameters of the functions it inlines (frames.cpp may give their frames the
// same bytes, as they never run at the same time); -1 for its own, globals and fixed places
std::vector<std::int32_t> FrameSharers(const Backend& be, std::int32_t function) {
    std::vector<std::int32_t> out(be.info.size(), -1);
    for (std::size_t s = 0; s < be.info.size(); s++) {
        const SymInfo& info = be.info[s];
        if (info.role != SymRole::Var) {
            continue;
        }
        const Var& var = be.program->vars[static_cast<std::size_t>(info.owner)];
        if (var.function >= 0 && var.function != function && !var.placement.has_address) {
            out[s] = var.function;
        }
    }
    return out;
}

} // namespace tisple
