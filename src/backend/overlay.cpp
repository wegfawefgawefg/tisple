// Overlay groups in the generated code: overlay.hpp.

#include "backend/overlay.hpp"

#include "backend/aux_code.hpp"
#include "backend/irq.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace tisple {

namespace {

constexpr std::uint16_t kRamrdOn = 0xC003;
constexpr std::uint16_t kRamrdOff = 0xC002;

// a JSR, a JMP to a label, or a branch: control goes to the operand
bool IsTransfer(const AsmItem& item) {
    if (item.kind != ItemKind::Ins) {
        return false;
    }
    return item.mn == Mn::JSR || (item.mn == Mn::JMP && item.mode == Mode::Abs) ||
           item.mode == Mode::Rel;
}

struct Code {
    const Backend* be = nullptr;
    const std::vector<bool>* kept = nullptr;
    std::map<std::int32_t, std::int32_t> owner; // by label: the function whose code has it
    std::vector<std::set<std::int32_t>> refs;   // by function: the functions it names or calls
    std::vector<bool> barred;
    std::vector<bool> always; // reached without any call of the program's: main, ...
};

Code ReadCode(const Backend& be, const std::vector<bool>& kept, const std::vector<AsmItem>& startup,
              const std::vector<AsmItem>& runtime) {
    const Program& program = *be.program;
    std::size_t count = program.functions.size();
    Code c;
    c.be = &be;
    c.kept = &kept;
    for (std::size_t f = 0; f < count; f++) {
        for (const AsmItem& item : be.code[f]) {
            if (kept[f] && item.kind == ItemKind::Label) {
                c.owner[item.value.sym] = static_cast<std::int32_t>(f);
            }
        }
    }
    c.refs.assign(count, {});
    for (std::size_t f = 0; f < count; f++) {
        if (!kept[f]) {
            continue;
        }
        for (const AsmItem& item : be.code[f]) {
            auto found = item.value.sym >= 0 ? c.owner.find(item.value.sym) : c.owner.end();
            if (found != c.owner.end() && found->second != static_cast<std::int32_t>(f)) {
                c.refs[f].insert(found->second);
            }
        }
        for (std::int32_t callee : program.functions[f].callees) {
            if (kept[static_cast<std::size_t>(callee)]) {
                c.refs[f].insert(callee);
            }
        }
    }
    std::vector<bool> handler = HandlerFunctions(program);
    std::vector<bool> entry(count, false);
    for (std::int32_t e : be.entries) {
        if (e >= 0) {
            entry[static_cast<std::size_t>(e)] = true;
        }
    }
    c.barred.assign(count, false);
    c.always.assign(count, false);
    for (std::size_t f = 0; f < count; f++) {
        const Function& fn = program.functions[f];
        std::int32_t id = static_cast<std::int32_t>(f);
        bool aux = IsAuxFunction(be.aux, id) || IsMirroredFunction(be.aux, id);
        c.always[f] = id == program.main || id == program.handler || entry[f] || fn.dispatcher ||
                      fn.io_patch >= 0;
        c.barred[f] = !kept[f] || be.code[f].empty() || handler[f] || c.always[f] || aux;
    }
    // what the startup and the runtime name is reached too
    for (const std::vector<AsmItem>* piece : {&startup, &runtime}) {
        for (const AsmItem& item : *piece) {
            auto found = item.value.sym >= 0 ? c.owner.find(item.value.sym) : c.owner.end();
            if (found != c.owner.end()) {
                c.always[static_cast<std::size_t>(found->second)] = true;
                c.barred[static_cast<std::size_t>(found->second)] = true;
            }
        }
    }
    return c;
}

// the kept functions reached only through roots, the roots included, but the barred ones
std::vector<bool> MembersOf(const Code& c, const std::vector<std::int32_t>& roots) {
    std::size_t count = c.refs.size();
    std::vector<bool> blocked(count, false);
    for (std::int32_t r : roots) {
        blocked[static_cast<std::size_t>(r)] = true;
    }
    std::vector<bool> outside(count, false);
    std::vector<std::int32_t> work;
    for (std::size_t f = 0; f < count; f++) {
        if (c.always[f] && !blocked[f]) {
            outside[f] = true;
            work.push_back(static_cast<std::int32_t>(f));
        }
    }
    while (!work.empty()) {
        std::size_t f = static_cast<std::size_t>(work.back());
        work.pop_back();
        for (std::int32_t g : c.refs[f]) {
            std::size_t i = static_cast<std::size_t>(g);
            if (!outside[i] && !blocked[i]) {
                outside[i] = true;
                work.push_back(g);
            }
        }
    }
    // a barred function stays resident, and so does what only it calls
    std::vector<bool> members(count, false);
    work = roots;
    while (!work.empty()) {
        std::size_t f = static_cast<std::size_t>(work.back());
        work.pop_back();
        if (members[f] || outside[f] || c.barred[f]) {
            continue;
        }
        members[f] = true;
        for (std::int32_t g : c.refs[f]) {
            work.push_back(g);
        }
    }
    return members;
}

// every function a group's code may run, through resident code too
std::vector<bool> RunsFrom(const Code& c, const std::vector<bool>& members) {
    std::vector<bool> seen(members.size(), false);
    std::vector<std::int32_t> work;
    for (std::size_t f = 0; f < members.size(); f++) {
        if (members[f]) {
            work.push_back(static_cast<std::int32_t>(f));
        }
    }
    while (!work.empty()) {
        std::size_t f = static_cast<std::size_t>(work.back());
        work.pop_back();
        for (std::int32_t g : c.refs[f]) {
            if (!seen[static_cast<std::size_t>(g)]) {
                seen[static_cast<std::size_t>(g)] = true;
                work.push_back(g);
            }
        }
    }
    return seen;
}

// a reference from code outside group g (in other, -1 resident) to a label of member m is a call
// or jump to a root's entry
bool Allowed(const Code& c, const OverlayGroups& groups, std::size_t g, const AsmItem& item,
             std::int32_t m, std::int32_t other) {
    if (other >= 0) {
        return false; // another group's code: it would be overwritten while it waits
    }
    const std::vector<std::int32_t>& roots = groups.roots[g];
    bool root = std::find(roots.begin(), roots.end(), m) != roots.end();
    return root && IsTransfer(item) && item.value.offset == 0 && item.value.part == Part::Whole &&
           item.value.sym == c.be->fn_sym[static_cast<std::size_t>(m)];
}

// the group whose code breaks the rules, or -1
std::int32_t FindBroken(const Code& c, const OverlayGroups& groups,
                        const std::vector<AsmItem>& startup, const std::vector<AsmItem>& runtime) {
    const Backend& be = *c.be;
    std::size_t count = groups.group.size();
    auto check = [&](const std::vector<AsmItem>& items, std::int32_t from) {
        std::int32_t from_group = from >= 0 ? groups.group[static_cast<std::size_t>(from)] : -1;
        for (const AsmItem& item : items) {
            auto found = item.value.sym >= 0 ? c.owner.find(item.value.sym) : c.owner.end();
            if (found == c.owner.end()) {
                continue;
            }
            std::int32_t g = groups.group[static_cast<std::size_t>(found->second)];
            if (g < 0 || g == from_group) {
                continue;
            }
            if (!Allowed(c, groups, static_cast<std::size_t>(g), item, found->second, from_group)) {
                return g;
            }
        }
        return -1;
    };
    for (const std::vector<AsmItem>* piece : {&startup, &runtime}) {
        std::int32_t g = check(*piece, -1);
        if (g >= 0) {
            return g;
        }
    }
    for (std::size_t f = 0; f < count; f++) {
        std::int32_t g = (*c.kept)[f] ? check(be.code[f], static_cast<std::int32_t>(f)) : -1;
        if (g >= 0) {
            return g;
        }
    }
    // a group whose code runs another's root, through resident code: the smaller goes
    for (std::size_t g = 0; g < groups.roots.size(); g++) {
        std::vector<bool> members(count, false);
        for (std::int32_t m : groups.members[g]) {
            members[static_cast<std::size_t>(m)] = true;
        }
        std::vector<bool> runs = RunsFrom(c, members);
        for (std::size_t h = 0; h < groups.roots.size(); h++) {
            for (std::int32_t r : groups.roots[h]) {
                if (h != g && runs[static_cast<std::size_t>(r)]) {
                    return static_cast<std::int32_t>(groups.bytes[g] < groups.bytes[h] ? g : h);
                }
            }
        }
    }
    return -1;
}

std::uint32_t CodeBytes(const Backend& be, std::int32_t f) {
    std::uint32_t bytes = 0;
    for (const AsmItem& item : be.code[static_cast<std::size_t>(f)]) {
        bytes += static_cast<std::uint32_t>(ItemBytes(be.as, item));
    }
    return bytes;
}

} // namespace

OverlayGroups FindOverlayGroups(const Backend& be, const std::vector<bool>& kept,
                                const OverlayPlan& plan, const std::vector<AsmItem>& startup,
                                const std::vector<AsmItem>& runtime, std::uint32_t min_bytes) {
    std::size_t count = be.program->functions.size();
    Code c = ReadCode(be, kept, startup, runtime);
    std::vector<std::vector<std::int32_t>> planned;
    for (const OverlayGroupPlan& group : plan.groups) {
        std::vector<std::int32_t> roots;
        for (std::int32_t r : group.roots) {
            if (!c.barred[static_cast<std::size_t>(r)]) {
                roots.push_back(r);
            }
        }
        if (!roots.empty()) {
            planned.push_back(roots);
        }
    }
    for (;;) {
        OverlayGroups out;
        out.group.assign(count, -1);
        for (const std::vector<std::int32_t>& roots : planned) {
            std::vector<bool> members = MembersOf(c, roots);
            std::vector<std::int32_t> list = roots;
            std::uint32_t bytes = 0;
            bool clash = false;
            for (std::size_t f = 0; f < count; f++) {
                if (!members[f]) {
                    continue;
                }
                clash = clash || out.group[f] >= 0;
                if (std::find(roots.begin(), roots.end(), f) == roots.end()) {
                    list.push_back(static_cast<std::int32_t>(f));
                }
                bytes += CodeBytes(be, static_cast<std::int32_t>(f));
            }
            if (clash) {
                continue; // a function two groups would have: the first keeps it
            }
            std::int32_t g = static_cast<std::int32_t>(out.roots.size());
            for (std::int32_t f : list) {
                out.group[static_cast<std::size_t>(f)] = g;
            }
            out.roots.push_back(roots);
            out.members.push_back(list);
            out.bytes.push_back(bytes);
        }
        std::int32_t broken = FindBroken(c, out, startup, runtime);
        for (std::size_t g = 0; g < out.bytes.size() && broken < 0; g++) {
            broken = out.bytes[g] < min_bytes ? static_cast<std::int32_t>(g) : -1;
        }
        if (broken < 0) {
            return out;
        }
        planned.erase(planned.begin() + broken);
    }
}

void RouteOverlays(Backend& be, const OverlayGroups& groups,
                   const std::vector<std::vector<AsmItem>*>& pieces,
                   const std::vector<std::int32_t>& piece_group, OverlayCode& code) {
    const Program& program = *be.program;
    std::size_t count = program.functions.size();
    std::size_t n = groups.roots.size();
    code = OverlayCode();
    code.stub.assign(count, -1);
    code.region_sym = NewSym(be, "ov.region", SymRole::Data, -1);
    std::vector<std::int32_t> load(count, -1);
    std::vector<std::int32_t> enter(n, -1);
    std::vector<std::int32_t> off(n, -1);
    for (std::size_t g = 0; g < n; g++) {
        std::string k = std::to_string(g);
        enter[g] = NewSym(be, "ov." + k + ".enter", SymRole::Label, -1);
        off[g] = NewSym(be, "ov." + k + ".off", SymRole::Label, -1);
        code.image_sym.push_back(NewSym(be, "ov." + k + ".image", SymRole::Data, -1));
        code.size_sym.push_back(NewSym(be, "ov." + k + ".size", SymRole::Data, -1));
        for (std::int32_t r : groups.roots[g]) {
            const std::string& name = program.functions[static_cast<std::size_t>(r)].name;
            code.stub[static_cast<std::size_t>(r)] = NewSym(be, "ov." + name, SymRole::Label, -1);
            load[static_cast<std::size_t>(r)] =
                NewSym(be, "ov." + name + ".load", SymRole::Label, -1);
        }
    }
    std::int32_t copy = NewSym(be, "ov.copy", SymRole::Label, -1);
    std::vector<AsmItem>& out = code.stubs;
    // the stubs, each with its load
    for (std::size_t g = 0; g < n; g++) {
        for (std::int32_t r : groups.roots[g]) {
            std::size_t f = static_cast<std::size_t>(r);
            out.push_back(MakeLabel(code.stub[f]));
            out.push_back(MakeIns(Mn::JMP, Mode::Abs, SymValue(load[f])));
            out.push_back(MakeLabel(load[f]));
            out.push_back(MakeIns(Mn::JSR, Mode::Abs, SymValue(enter[g])));
            out.push_back(MakeIns(Mn::JMP, Mode::Abs, SymValue(be.fn_sym[f])));
        }
    }
    // a group's stubs pointed at its roots (on) or their loads (off)
    auto point = [&](std::size_t g, bool on) {
        for (std::int32_t r : groups.roots[g]) {
            std::size_t f = static_cast<std::size_t>(r);
            std::int32_t to = on ? be.fn_sym[f] : load[f];
            out.push_back(MakeIns(Mn::LDA, Mode::Imm, SymValue(to, 0, Part::Low)));
            out.push_back(MakeIns(Mn::STA, Mode::Abs, SymValue(code.stub[f], 1)));
            out.push_back(MakeIns(Mn::LDA, Mode::Imm, SymValue(to, 0, Part::High)));
            out.push_back(MakeIns(Mn::STA, Mode::Abs, SymValue(code.stub[f], 2)));
        }
    };
    for (std::size_t g = 0; g < n; g++) {
        out.push_back(MakeLabel(off[g]));
        point(g, false);
        out.push_back(MakeIns(Mn::RTS, Mode::Imp));
    }
    std::int32_t rt_a = be.rt_a;
    std::int32_t rt_b = be.rt_b;
    std::int32_t rt_r = be.rt_r;
    for (std::size_t g = 0; g < n; g++) {
        out.push_back(MakeLabel(enter[g]));
        for (Mn mn : {Mn::PHP, Mn::PHA, Mn::TXA, Mn::PHA, Mn::TYA, Mn::PHA}) {
            out.push_back(MakeIns(mn, Mode::Imp));
        }
        for (std::size_t h = 0; h < n; h++) {
            if (h != g) {
                out.push_back(MakeIns(Mn::JSR, Mode::Abs, SymValue(off[h])));
            }
        }
        std::int32_t image = code.image_sym[g];
        out.push_back(MakeIns(Mn::LDA, Mode::Imm, SymValue(image, 0, Part::Low)));
        out.push_back(MakeIns(Mn::STA, Mode::Abs, SymValue(rt_a)));
        out.push_back(MakeIns(Mn::LDA, Mode::Imm, SymValue(image, 0, Part::High)));
        out.push_back(MakeIns(Mn::STA, Mode::Abs, SymValue(rt_a, 1)));
        out.push_back(MakeIns(Mn::LDA, Mode::Imm, SymValue(code.region_sym, 0, Part::Low)));
        out.push_back(MakeIns(Mn::STA, Mode::Abs, SymValue(rt_b)));
        out.push_back(MakeIns(Mn::LDA, Mode::Imm, SymValue(code.region_sym, 0, Part::High)));
        out.push_back(MakeIns(Mn::STA, Mode::Abs, SymValue(rt_b, 1)));
        out.push_back(MakeIns(Mn::LDA, Mode::Imm, SymValue(code.size_sym[g], 0, Part::High)));
        out.push_back(MakeIns(Mn::STA, Mode::Abs, SymValue(rt_r)));
        out.push_back(MakeIns(Mn::LDX, Mode::Imm, SymValue(code.size_sym[g], 0, Part::Low)));
        out.push_back(MakeIns(Mn::JSR, Mode::Abs, SymValue(copy)));
        point(g, true);
        for (Mn mn : {Mn::PLA, Mn::TAY, Mn::PLA, Mn::TAX, Mn::PLA, Mn::PLP, Mn::RTS}) {
            out.push_back(MakeIns(mn, Mode::Imp));
        }
    }
    // the copy: X whole pages, then rt_r bytes, from (rt_a) in aux memory to (rt_b)
    std::vector<AsmItem>& c = code.copy;
    std::int32_t page = NewSym(be, "ov.copy.page", SymRole::Label, -1);
    std::int32_t byte = NewSym(be, "ov.copy.byte", SymRole::Label, -1);
    std::int32_t tail = NewSym(be, "ov.copy.tail", SymRole::Label, -1);
    std::int32_t last = NewSym(be, "ov.copy.last", SymRole::Label, -1);
    std::int32_t done = NewSym(be, "ov.copy.done", SymRole::Label, -1);
    c.push_back(MakeLabel(copy));
    c.push_back(MakeIns(Mn::LDY, Mode::Imm, Num(0)));
    c.push_back(MakeIns(Mn::CPX, Mode::Imm, Num(0)));
    c.push_back(MakeIns(Mn::BEQ, Mode::Rel, SymValue(tail)));
    c.push_back(MakeLabel(page));
    c.push_back(MakeIns(Mn::PHP, Mode::Imp));
    c.push_back(MakeIns(Mn::SEI, Mode::Imp));
    c.push_back(MakeIns(Mn::STA, Mode::Abs, Num(kRamrdOn)));
    c.push_back(MakeLabel(byte));
    c.push_back(MakeIns(Mn::LDA, Mode::IndY, SymValue(rt_a)));
    c.push_back(MakeIns(Mn::STA, Mode::IndY, SymValue(rt_b)));
    c.push_back(MakeIns(Mn::INY, Mode::Imp));
    c.push_back(MakeIns(Mn::BNE, Mode::Rel, SymValue(byte)));
    c.push_back(MakeIns(Mn::STA, Mode::Abs, Num(kRamrdOff)));
    c.push_back(MakeIns(Mn::PLP, Mode::Imp));
    c.push_back(MakeIns(Mn::INC, Mode::Abs, SymValue(rt_a, 1)));
    c.push_back(MakeIns(Mn::INC, Mode::Abs, SymValue(rt_b, 1)));
    c.push_back(MakeIns(Mn::DEX, Mode::Imp));
    c.push_back(MakeIns(Mn::BNE, Mode::Rel, SymValue(page)));
    c.push_back(MakeLabel(tail));
    c.push_back(MakeIns(Mn::LDY, Mode::Abs, SymValue(rt_r)));
    c.push_back(MakeIns(Mn::BEQ, Mode::Rel, SymValue(done)));
    c.push_back(MakeIns(Mn::PHP, Mode::Imp));
    c.push_back(MakeIns(Mn::SEI, Mode::Imp));
    c.push_back(MakeIns(Mn::STA, Mode::Abs, Num(kRamrdOn)));
    c.push_back(MakeLabel(last));
    c.push_back(MakeIns(Mn::DEY, Mode::Imp));
    c.push_back(MakeIns(Mn::LDA, Mode::IndY, SymValue(rt_a)));
    c.push_back(MakeIns(Mn::STA, Mode::IndY, SymValue(rt_b)));
    c.push_back(MakeIns(Mn::TYA, Mode::Imp));
    c.push_back(MakeIns(Mn::BNE, Mode::Rel, SymValue(last)));
    c.push_back(MakeIns(Mn::STA, Mode::Abs, Num(kRamrdOff)));
    c.push_back(MakeIns(Mn::PLP, Mode::Imp));
    c.push_back(MakeLabel(done));
    c.push_back(MakeIns(Mn::RTS, Mode::Imp));
    // every call or jump to a root from outside its group to its stub
    for (std::size_t p = 0; p < pieces.size(); p++) {
        for (AsmItem& item : *pieces[p]) {
            if (!IsTransfer(item) || item.value.sym < 0 || item.value.offset != 0) {
                continue;
            }
            for (std::size_t g = 0; g < n; g++) {
                if (static_cast<std::int32_t>(g) == piece_group[p]) {
                    continue;
                }
                for (std::int32_t r : groups.roots[g]) {
                    if (item.value.sym == be.fn_sym[static_cast<std::size_t>(r)]) {
                        item.value = SymValue(code.stub[static_cast<std::size_t>(r)]);
                    }
                }
            }
        }
    }
}

bool PlaceOverlayImages(const std::vector<AddressRange>& taken, std::uint32_t size,
                        std::uint32_t& at) {
    // aux memory's text page and hi-res page 1 follow PAGE2 while 80STORE is on
    std::vector<AddressRange> used = taken;
    used.push_back(AddressRange{0x0000, kAuxDataStart});
    used.push_back(AddressRange{0x2000, 0x4000});
    used.push_back(AddressRange{kAuxDataEnd, 0x10000});
    std::vector<std::uint32_t> starts = {kAuxDataStart};
    for (const AddressRange& range : used) {
        starts.push_back(range.end);
    }
    std::sort(starts.begin(), starts.end());
    for (std::uint32_t start : starts) {
        bool free = start + size <= kAuxDataEnd;
        for (const AddressRange& range : used) {
            free = free && (start + size <= range.start || start >= range.end);
        }
        if (free) {
            at = start;
            return true;
        }
    }
    return false;
}

std::uint64_t OverlayLoadCycles(std::uint32_t bytes, std::size_t stubs) {
    // 16 a byte, a page's set-up, the enter routine and 20 a stub patched
    return 16ull * bytes + 40ull * (bytes / 256 + 1) + 80 + 20ull * stubs;
}

} // namespace tisple
