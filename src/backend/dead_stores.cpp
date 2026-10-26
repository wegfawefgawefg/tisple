#include "asm/opcodes.hpp"
#include "backend/peephole.hpp"

#include <map>

// Stores to the function's own bytes that nothing reads before they are written again or the
// function returns. Liveness runs backwards over the items until it settles; a byte read by
// an indexed or indirect mode, or whose address is taken (#<sym), counts as always live.

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

} // namespace

bool DropDeadStores(std::vector<AsmItem>& items, const std::vector<bool>& private_syms) {
    // private bytes read one at a time get numbers; a symbol read any other way is left alone
    std::vector<bool> whole(private_syms.size(), false);
    Flow f;
    f.items = &items;
    for (std::size_t i = 0; i < items.size(); i++) {
        const AsmItem& item = items[i];
        if (item.kind == ItemKind::Label) {
            f.labels[item.value.sym] = i;
        }
        std::size_t sym = static_cast<std::size_t>(item.value.sym);
        if (item.value.sym < 0 || sym >= private_syms.size() || !private_syms[sym]) {
            continue;
        }
        if (item.kind != ItemKind::Ins || !IsPlain(item)) {
            whole[sym] = true;
        }
    }
    for (const AsmItem& item : items) {
        if (item.kind == ItemKind::Byte && item.value.sym >= 0 && f.labels.count(item.value.sym)) {
            f.dispatch.push_back(f.labels[item.value.sym]);
        }
    }
    std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> numbers;
    std::vector<std::size_t> byte_of(items.size(), kNone);
    for (std::size_t i = 0; i < items.size(); i++) {
        const AsmItem& item = items[i];
        std::size_t sym = static_cast<std::size_t>(item.value.sym);
        if (item.kind != ItemKind::Ins || item.value.sym < 0 || sym >= private_syms.size() ||
            !private_syms[sym] || whole[sym]) {
            continue;
        }
        auto key = std::make_pair(item.value.sym, item.value.offset);
        auto found = numbers.find(key);
        if (found == numbers.end()) {
            found = numbers.emplace(key, numbers.size()).first;
        }
        byte_of[i] = found->second;
    }
    if (numbers.empty()) {
        return false;
    }
    std::size_t words = (numbers.size() + 63) / 64;
    Bits empty;
    empty.words.assign(words, 0);
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
                }
            }
            Bits in = live_out[i];
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

} // namespace tisple
