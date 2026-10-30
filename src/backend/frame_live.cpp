#include "backend/frame_live.hpp"

#include <algorithm>

// Bytes are numbered per symbol followed: every candidate's (Var::made locals frame_share.cpp
// may move), and those of other functions' frames (an inlined callee's locals, a callee's
// parameters set before its call). The function's own other bytes are left out: they are in
// its own frame, which no frame a candidate's slot may go to overlaps.
// - A plain store kills a byte; a load, an ALU operand or a read-modify-write reads it; an
//   indexed access reads or writes some byte of the symbol (a write kills nothing); (zp),Y
//   and JMP (ind) read the pointer's two bytes; a variable wait reads its u16.
// - A JSR reads its callee's parameters (a function not known: every exposed byte); a tail
//   call likewise; a return reads other functions' temporaries, if any show; a jump or branch
//   out to somewhere not known reads every exposed byte.
// - A symbol reached any other way (its address as a number, (zp,X), a data byte, an offset
//   outside it) holds a value everywhere in this code, and a candidate so reached is pinned.

namespace tisple {

namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

// a set of numbered bytes
struct Bits {
    std::vector<std::uint64_t> words;

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

template <typename Fn> void EachBit(const Bits& bits, Fn fn) {
    for (std::size_t w = 0; w < bits.words.size(); w++) {
        std::uint64_t word = bits.words[w];
        while (word != 0) {
            int b = __builtin_ctzll(word);
            word &= word - 1;
            fn(w * 64 + static_cast<std::size_t>(b));
        }
    }
}

// what an item does to the bytes of one frame symbol
struct Touch {
    std::int32_t sym = -1; // -1: none followed
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    bool read = false;
    bool write = false;
    bool kills = false; // a plain store: the byte holds a new value
    bool odd = false;   // reached in a way the liveness does not follow
};

bool IsStore(Mn mn) {
    return mn == Mn::STA || mn == Mn::STX || mn == Mn::STY;
}

bool IsReadModifyWrite(Mn mn) {
    return mn == Mn::ASL || mn == Mn::LSR || mn == Mn::ROL || mn == Mn::ROR || mn == Mn::INC ||
           mn == Mn::DEC;
}

void Range(Touch& t, std::int32_t first, std::uint32_t count, std::uint32_t size) {
    if (first < 0 || static_cast<std::uint32_t>(first) + count > size) {
        t.odd = true;
        return;
    }
    t.first = static_cast<std::uint32_t>(first);
    t.count = count;
}

Touch TouchOf(const FrameFacts& facts, const AsmItem& item) {
    Touch t;
    std::int32_t sym = item.value.sym;
    if (item.kind == ItemKind::Label || item.kind == ItemKind::Note || sym < 0 ||
        static_cast<std::size_t>(sym) >= facts.syms.size() ||
        facts.syms[static_cast<std::size_t>(sym)].owner < 0) {
        return t;
    }
    t.sym = sym;
    std::uint32_t size = facts.syms[static_cast<std::size_t>(sym)].size;
    if (item.kind == ItemKind::Wait && item.mode == Mode::Abs && item.value.part == Part::Whole) {
        t.read = true;
        Range(t, item.value.offset, 2, size);
        return t;
    }
    if (item.kind != ItemKind::Ins || item.value.part != Part::Whole) {
        t.odd = true;
        return t;
    }
    bool store = IsStore(item.mn);
    switch (item.mode) {
    case Mode::Zp:
    case Mode::Abs:
        if (item.mn == Mn::JMP || item.mn == Mn::JSR) {
            t.odd = true;
            return t;
        }
        t.read = !store;
        t.write = store || IsReadModifyWrite(item.mn);
        t.kills = store;
        Range(t, item.value.offset, 1, size);
        return t;
    case Mode::ZpX:
    case Mode::ZpY:
    case Mode::AbsX:
    case Mode::AbsY:
        t.read = !store;
        t.write = store || IsReadModifyWrite(item.mn);
        Range(t, 0, size, size);
        return t;
    case Mode::Ind:
    case Mode::IndY:
        t.read = true;
        Range(t, item.value.offset, 2, size);
        return t;
    default:
        t.odd = true;
        return t;
    }
}

// one code's liveness: the bytes followed and what calls and exits read of them
struct Live {
    const FrameFacts* facts = nullptr;
    const std::vector<AsmItem>* items = nullptr;
    std::map<std::int32_t, std::size_t> base; // sym -> its first byte's number
    std::vector<std::int32_t> sym_of_byte;
    std::map<std::int32_t, std::size_t> labels; // label symbol -> its item
    std::vector<std::size_t> dispatch;          // labels a jump table's bytes name
    Bits empty;
    Bits temps;                         // other functions' temporaries
    Bits exposed;                       // those and other functions' parameters
    std::map<std::int32_t, Bits> calls; // by function: what a call of it reads
};

// where control may go after item i (as dead_stores.cpp, but a data byte goes on to the next
// item); kNone stands for leaving this code
void Successors(const Live& l, std::size_t i, std::vector<std::size_t>& out) {
    out.clear();
    const AsmItem& item = (*l.items)[i];
    std::size_t next = i + 1 < l.items->size() ? i + 1 : kNone;
    auto target = [&](std::int32_t label) {
        auto found = l.labels.find(label);
        return found == l.labels.end() ? kNone : found->second;
    };
    if (item.kind == ItemKind::Ins) {
        switch (item.mn) {
        case Mn::JMP:
            if (item.mode == Mode::Abs) {
                out.push_back(target(item.value.sym));
                return;
            }
            for (const auto& label : l.labels) {
                out.push_back(label.second);
            }
            out.push_back(kNone);
            return;
        case Mn::RTS:
            // a match pushes a table entry and returns into it
            out.insert(out.end(), l.dispatch.begin(), l.dispatch.end());
            out.push_back(kNone);
            return;
        case Mn::RTI:
        case Mn::BRK:
            out.push_back(kNone);
            return;
        default:
            break;
        }
        if (IsBranch(item.mn)) {
            out.push_back(target(item.value.sym));
        }
    }
    out.push_back(next);
}

// what a JSR or JMP to target reads (nothing for the monitor rom and the runtime routines)
const Bits& CallReads(const Live& l, std::int32_t target) {
    const FrameFacts& facts = *l.facts;
    if (target < 0 || facts.routine[static_cast<std::size_t>(target)]) {
        return l.empty;
    }
    std::int32_t callee = facts.called[static_cast<std::size_t>(target)];
    if (callee < 0) {
        return l.exposed;
    }
    auto found = l.calls.find(callee);
    return found == l.calls.end() ? l.temps : found->second;
}

// what leaving this code after item i reads
const Bits& ExitReads(const Live& l, const AsmItem& item) {
    if (item.kind != ItemKind::Ins) {
        return l.temps;
    }
    if (item.mn == Mn::JMP && item.mode == Mode::Abs) {
        return CallReads(l, item.value.sym);
    }
    if (item.mn == Mn::JMP || IsBranch(item.mn)) {
        return l.exposed;
    }
    return l.temps;
}

void AddMeet(FrameFacts& facts, std::int32_t a, std::int32_t b) {
    if (facts.candidate[static_cast<std::size_t>(a)]) {
        facts.meets[a].insert(b);
    }
    if (facts.candidate[static_cast<std::size_t>(b)]) {
        facts.meets[b].insert(a);
    }
}

void NumberBytes(Live& l) {
    const FrameFacts& facts = *l.facts;
    l.empty.words.assign((l.sym_of_byte.size() + 63) / 64, 0);
    l.temps = l.empty;
    l.exposed = l.empty;
    for (std::size_t b = 0; b < l.sym_of_byte.size(); b++) {
        const FrameSym& sym = facts.syms[static_cast<std::size_t>(l.sym_of_byte[b])];
        if (sym.temps) {
            l.temps.Set(b);
            l.exposed.Set(b);
        }
        if (sym.param) {
            l.exposed.Set(b);
            if (l.calls.find(sym.owner) == l.calls.end()) {
                l.calls[sym.owner] = l.empty;
            }
            l.calls[sym.owner].Set(b);
        }
    }
    for (auto& [callee, reads] : l.calls) {
        reads.Merge(l.temps);
    }
    for (std::size_t i = 0; i < l.items->size(); i++) {
        const AsmItem& item = (*l.items)[i];
        if (item.kind == ItemKind::Label) {
            l.labels[item.value.sym] = i;
        }
    }
    for (const AsmItem& item : *l.items) {
        if (item.kind == ItemKind::Byte && item.value.sym >= 0 && l.labels.count(item.value.sym)) {
            l.dispatch.push_back(l.labels[item.value.sym]);
        }
    }
}

// backwards over the items until nothing changes
void Solve(const Live& l, const std::vector<Touch>& touches, std::vector<Bits>& live_in,
           std::vector<Bits>& live_out) {
    const std::vector<AsmItem>& items = *l.items;
    live_in.assign(items.size(), l.empty);
    live_out.assign(items.size(), l.empty);
    std::vector<std::size_t> succ;
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = items.size(); i-- > 0;) {
            Successors(l, i, succ);
            Bits out = l.empty;
            for (std::size_t n : succ) {
                out.Merge(n == kNone ? ExitReads(l, items[i]) : live_in[n]);
            }
            Bits in = out;
            if (items[i].kind == ItemKind::Ins && items[i].mn == Mn::JSR) {
                in.Merge(CallReads(l, items[i].value.sym));
            }
            const Touch& t = touches[i];
            if (t.sym >= 0) {
                std::size_t at = l.base.at(t.sym) + t.first;
                for (std::size_t b = at; b < at + t.count; b++) {
                    if (t.kills) {
                        in.Clear(b);
                    }
                    if (t.read) {
                        in.Set(b);
                    }
                }
            }
            live_out[i] = out;
            changed = live_in[i].Merge(in) || changed;
        }
    }
}

// what the liveness says: who meets whom, what is called while a candidate holds a value
void Learn(FrameFacts& facts, const Live& l, const std::vector<Touch>& touches,
           const std::vector<Bits>& live_in, const std::vector<Bits>& live_out) {
    // a candidate holding a value where the code starts: from an earlier run, or never set
    EachBit(live_in[0], [&](std::size_t b) {
        std::size_t sym = static_cast<std::size_t>(l.sym_of_byte[b]);
        if (facts.candidate[sym]) {
            facts.pinned[sym] = true;
        }
    });
    const std::vector<AsmItem>& items = *l.items;
    for (std::size_t i = 0; i < items.size(); i++) {
        const Touch& t = touches[i];
        if (t.sym >= 0 && t.write) {
            EachBit(live_out[i], [&](std::size_t b) {
                std::int32_t other = l.sym_of_byte[b];
                if (other != t.sym) {
                    AddMeet(facts, t.sym, other);
                }
            });
        }
        const AsmItem& item = items[i];
        if (item.kind != ItemKind::Ins || item.mn != Mn::JSR) {
            continue;
        }
        std::int32_t target = item.value.sym;
        if (target < 0 || facts.routine[static_cast<std::size_t>(target)]) {
            continue;
        }
        std::int32_t callee = facts.called[static_cast<std::size_t>(target)];
        EachBit(live_out[i], [&](std::size_t b) {
            std::int32_t sym = l.sym_of_byte[b];
            if (facts.candidate[static_cast<std::size_t>(sym)]) {
                facts.calls[sym].insert(callee);
            }
        });
    }
}

} // namespace

void LookAtCode(FrameFacts& facts, std::int32_t function, const std::vector<AsmItem>& items) {
    Live l;
    l.facts = &facts;
    l.items = &items;
    std::vector<std::int32_t> odd;
    std::vector<Touch> touches(items.size());
    for (std::size_t i = 0; i < items.size(); i++) {
        Touch t = TouchOf(facts, items[i]);
        if (t.sym < 0) {
            continue;
        }
        std::size_t sym = static_cast<std::size_t>(t.sym);
        facts.named[sym] = true;
        if (facts.syms[sym].owner == function && !facts.candidate[sym]) {
            continue;
        }
        if (t.odd) {
            if (facts.candidate[sym]) {
                facts.pinned[sym] = true;
            }
            if (std::find(odd.begin(), odd.end(), t.sym) == odd.end()) {
                odd.push_back(t.sym);
            }
            continue;
        }
        if (l.base.find(t.sym) == l.base.end()) {
            l.base[t.sym] = l.sym_of_byte.size();
            l.sym_of_byte.insert(l.sym_of_byte.end(), facts.syms[sym].size, t.sym);
        }
        touches[i] = t;
    }
    for (const auto& [sym, at] : l.base) {
        if (facts.candidate[static_cast<std::size_t>(sym)]) {
            facts.code_of[sym].insert(function);
        }
    }
    // a symbol reached in a way not followed holds a value everywhere
    for (std::int32_t o : odd) {
        for (const auto& [sym, at] : l.base) {
            AddMeet(facts, sym, o);
        }
        for (std::int32_t other : odd) {
            if (other != o) {
                AddMeet(facts, other, o);
            }
        }
        if (facts.candidate[static_cast<std::size_t>(o)]) {
            facts.code_of[o].insert(function);
        }
    }
    if (l.sym_of_byte.empty()) {
        return;
    }
    NumberBytes(l);
    std::vector<Bits> live_in;
    std::vector<Bits> live_out;
    Solve(l, touches, live_in, live_out);
    Learn(facts, l, touches, live_in, live_out);
}

} // namespace tisple
