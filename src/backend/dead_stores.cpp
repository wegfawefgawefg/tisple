#include "asm/opcodes.hpp"
#include "backend/peephole.hpp"

#include <map>

// Stores that nothing reads before the byte is written again, and steps (INC, DEC) of bytes
// nothing reads after them but more such steps: a u16 index whose loop reads its arrays through
// step pointers (gen_step.cpp), dead after the loop, is still stepped by its own code. Liveness
// runs backwards over the items until it settles. A private byte is dead when the function returns;
// an exposed one (a global, another function's parameter) is read by every call, jump out of the
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

// the tracked bytes of a function's items, numbered, and the flow between the items
struct Bytes {
    Flow f;
    std::vector<std::size_t> byte_of; // by item: the byte it reads or writes directly, or kNone
    Bits empty;
    Bits exposed; // what a call, a jump out and the return read
};

// false if no byte is tracked
bool NumberBytes(const std::vector<AsmItem>& items, const std::vector<StoreScope>& scopes,
                 Bytes& b) {
    // tracked bytes read one at a time get numbers; a symbol read any other way is left alone
    auto tracked = [&](std::int32_t sym) {
        return sym >= 0 && static_cast<std::size_t>(sym) < scopes.size() &&
               scopes[static_cast<std::size_t>(sym)] != StoreScope::Untracked;
    };
    std::vector<bool> whole(scopes.size(), false);
    b.f.items = &items;
    for (std::size_t i = 0; i < items.size(); i++) {
        const AsmItem& item = items[i];
        if (item.kind == ItemKind::Label) {
            b.f.labels[item.value.sym] = i;
        }
        if (!tracked(item.value.sym)) {
            continue;
        }
        if (item.kind != ItemKind::Ins || !IsPlain(item)) {
            whole[static_cast<std::size_t>(item.value.sym)] = true;
        }
    }
    for (const AsmItem& item : items) {
        if (item.kind == ItemKind::Byte && item.value.sym >= 0 &&
            b.f.labels.count(item.value.sym)) {
            b.f.dispatch.push_back(b.f.labels[item.value.sym]);
        }
    }
    std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> numbers;
    std::vector<bool> exposed_numbers;
    b.byte_of.assign(items.size(), kNone);
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
        b.byte_of[i] = found->second;
    }
    if (numbers.empty()) {
        return false;
    }
    b.empty.words.assign((numbers.size() + 63) / 64, 0);
    b.exposed = b.empty;
    for (std::size_t n = 0; n < exposed_numbers.size(); n++) {
        if (exposed_numbers[n]) {
            b.exposed.Set(n);
        }
    }
    return true;
}

// the bytes live after each item, as `words` 64-bit words an item
struct LiveSets {
    std::size_t words = 0;
    std::vector<std::uint64_t> out;

    bool Has(std::size_t item, std::size_t byte) const {
        return (out[item * words + byte / 64] >> (byte % 64)) & 1;
    }
};

// the bytes live after each item; an item `faint` marks (an INC or DEC) neither reads nor
// writes its byte. Flat arrays and the successors worked out once: the dead-store pass runs
// in every peephole round of every function
LiveSets LiveOut(const std::vector<AsmItem>& items, const Bytes& b,
                 const std::vector<bool>& faint) {
    std::size_t n = items.size();
    std::size_t words = b.empty.words.size();
    std::vector<std::size_t> first(n + 1, 0);
    std::vector<std::size_t> succ_all;
    std::vector<std::size_t> succ;
    for (std::size_t i = 0; i < n; i++) {
        Successors(b.f, i, succ);
        first[i] = succ_all.size();
        succ_all.insert(succ_all.end(), succ.begin(), succ.end());
    }
    first[n] = succ_all.size();
    LiveSets live;
    live.words = words;
    live.out.assign(n * words, 0);
    std::vector<std::uint64_t> live_in(n * words, 0);
    std::vector<std::uint64_t> in(words, 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = n; i-- > 0;) {
            std::uint64_t* out = &live.out[i * words];
            for (std::size_t k = first[i]; k < first[i + 1]; k++) {
                std::size_t s = succ_all[k];
                const std::uint64_t* from =
                    s != kNone ? &live_in[s * words] : b.exposed.words.data();
                for (std::size_t w = 0; w < words; w++) {
                    out[w] |= from[w];
                }
            }
            std::copy(out, out + words, in.begin());
            if (items[i].kind == ItemKind::Ins && items[i].mn == Mn::JSR) {
                for (std::size_t w = 0; w < words; w++) {
                    in[w] |= b.exposed.words[w];
                }
            }
            std::size_t byte = b.byte_of[i];
            if (byte != kNone && !faint[i]) {
                std::uint64_t bit = std::uint64_t(1) << (byte % 64);
                if (IsStore(items[i].mn)) {
                    in[byte / 64] &= ~bit;
                } else {
                    in[byte / 64] |= bit;
                }
            }
            std::uint64_t* at = &live_in[i * words];
            for (std::size_t w = 0; w < words; w++) {
                std::uint64_t both = at[w] | in[w];
                changed = changed || both != at[w];
                at[w] = both;
            }
        }
    }
    return live;
}

// the next item from i on that is not a note
std::size_t SkipNotes(const std::vector<AsmItem>& items, std::size_t i) {
    while (i < items.size() && items[i].kind == ItemKind::Note) {
        i++;
    }
    return i;
}

bool IsStep(const AsmItem& item) {
    return item.kind == ItemKind::Ins && (item.mn == Mn::INC || item.mn == Mn::DEC) &&
           IsPlain(item);
}

} // namespace

bool DropDeadStores(std::vector<AsmItem>& items, const std::vector<StoreScope>& scopes) {
    Bytes b;
    if (!NumberBytes(items, scopes, b)) {
        return false;
    }
    LiveSets live_out = LiveOut(items, b, std::vector<bool>(items.size(), false));
    std::vector<AsmItem> kept;
    bool dropped = false;
    for (std::size_t i = 0; i < items.size(); i++) {
        std::size_t byte = b.byte_of[i];
        if (byte != kNone && IsStore(items[i].mn) && !live_out.Has(i, byte)) {
            dropped = true;
            continue;
        }
        kept.push_back(items[i]);
    }
    items.swap(kept);
    return dropped;
}

bool DropFaintSteps(const Assembly& as, std::vector<AsmItem>& items,
                    const std::vector<StoreScope>& scopes) {
    // the steps that may go: an INC or DEC, and a 16-bit step up (INC lo / BNE L / INC hi / L).
    // by item: the group's last item, and where nothing may read N and Z (after the step, or
    // at L; a step leaves the carry as it was)
    std::vector<std::size_t> group_end(items.size(), kNone);
    std::vector<std::size_t> nz_at(items.size(), kNone);
    std::vector<bool> faint(items.size(), false);
    bool any = false;
    for (std::size_t i = 0; i < items.size(); i++) {
        if (!IsStep(items[i])) {
            continue;
        }
        std::size_t branch = SkipNotes(items, i + 1);
        if (branch >= items.size()) {
            continue;
        }
        bool up = items[i].mn == Mn::INC && items[branch].kind == ItemKind::Ins &&
                  items[branch].mn == Mn::BNE;
        std::size_t high = up ? SkipNotes(items, branch + 1) : kNone;
        std::size_t label = high < items.size() ? SkipNotes(items, high + 1) : kNone;
        bool pair = up && high < items.size() && IsStep(items[high]) && items[high].mn == Mn::INC &&
                    label < items.size() && items[label].kind == ItemKind::Label &&
                    items[label].value.sym == items[branch].value.sym;
        group_end[i] = pair ? high : i;
        nz_at[i] = pair ? label : branch;
        faint[i] = true;
        faint[group_end[i]] = true;
        any = true;
        i = group_end[i];
    }
    Bytes b;
    if (!any || !NumberBytes(items, scopes, b)) {
        return false;
    }
    // a group goes when nothing reads its bytes after it but more such steps, and nothing reads
    // the flags it leaves; one that stays reads its bytes, so the rest are looked at again
    std::vector<bool> gone(items.size(), false);
    std::vector<std::uint8_t> live;
    for (bool again = true; again;) {
        again = false;
        LiveSets live_out = LiveOut(items, b, faint);
        bool some = false;
        for (std::size_t i = 0; i < items.size(); i++) {
            if (!faint[i] || group_end[i] == kNone) {
                continue;
            }
            std::size_t last = group_end[i];
            bool dead = b.byte_of[i] != kNone && !live_out.Has(i, b.byte_of[i]) &&
                        b.byte_of[last] != kNone && !live_out.Has(last, b.byte_of[last]);
            if (dead && live.empty()) {
                live = LiveRegsIn(as, items);
            }
            dead = dead && (live[nz_at[i]] & (kLiveN | kLiveZ)) == 0;
            gone[i] = dead;
            some = some || dead;
            if (!dead) {
                faint[i] = false;
                faint[last] = false;
                again = true;
            }
        }
        if (!some) {
            return false;
        }
    }
    std::vector<AsmItem> kept;
    bool dropped = false;
    for (std::size_t i = 0; i < items.size(); i++) {
        if (!gone[i]) {
            kept.push_back(items[i]);
            continue;
        }
        // the step, the branch and the high byte's step; the label stays
        for (std::size_t k = i; k <= group_end[i]; k++) {
            if (items[k].kind == ItemKind::Note) {
                kept.push_back(items[k]);
            }
        }
        i = group_end[i];
        dropped = true;
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
