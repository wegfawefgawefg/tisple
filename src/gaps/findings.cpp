#include "gaps/findings.hpp"

#include <algorithm>
#include <map>

namespace tisple {

namespace {

struct SiteView {
    std::uint16_t address = 0;
    const SiteStats* s = nullptr;
    Mn mn = Mn::NOP;
    Mode mode = Mode::Imp;
    std::uint64_t avg_cycles = 0; // per execution, rounded down
};

bool Holds(std::uint64_t held, std::uint64_t out_of, const FindOptions& options) {
    if (out_of < options.min_count || held == 0) {
        return false;
    }
    return static_cast<double>(held) >= options.min_fraction * static_cast<double>(out_of);
}

const char* FlagReason(Mn mn) {
    switch (mn) {
    case Mn::CLC:
        return "carry already clear";
    case Mn::SEC:
        return "carry already set";
    case Mn::CLV:
        return "overflow already clear";
    case Mn::CLD:
        return "decimal already clear";
    case Mn::SED:
        return "decimal already set";
    case Mn::CLI:
        return "interrupts already enabled";
    default:
        return "interrupts already disabled";
    }
}

bool IsFlagOp(Mn mn) {
    return mn == Mn::CLC || mn == Mn::SEC || mn == Mn::CLV || mn == Mn::CLD || mn == Mn::SED ||
           mn == Mn::CLI || mn == Mn::SEI;
}

bool IsTransfer(Mn mn) {
    return mn == Mn::TAX || mn == Mn::TAY || mn == Mn::TXA || mn == Mn::TYA || mn == Mn::TSX ||
           mn == Mn::TXS;
}

bool IsLoad(Mn mn) {
    return mn == Mn::LDA || mn == Mn::LDX || mn == Mn::LDY;
}

bool IsStoreMn(Mn mn) {
    return mn == Mn::STA || mn == Mn::STX || mn == Mn::STY;
}

// instructions whose only effects are a register and flags
bool OnlyRegister(Mn mn, Mode mode) {
    switch (mn) {
    case Mn::LDA:
    case Mn::LDX:
    case Mn::LDY:
    case Mn::TAX:
    case Mn::TAY:
    case Mn::TXA:
    case Mn::TYA:
    case Mn::TSX:
    case Mn::INX:
    case Mn::INY:
    case Mn::DEX:
    case Mn::DEY:
    case Mn::AND:
    case Mn::ORA:
    case Mn::EOR:
    case Mn::ADC:
    case Mn::SBC:
        return true;
    case Mn::ASL:
    case Mn::LSR:
    case Mn::ROL:
    case Mn::ROR:
        return mode == Mode::Acc;
    default:
        return false;
    }
}

// instructions whose only effect is on flags
bool OnlyFlags(Mn mn) {
    return mn == Mn::CMP || mn == Mn::CPX || mn == Mn::CPY || mn == Mn::BIT || mn == Mn::CLC ||
           mn == Mn::SEC || mn == Mn::CLV;
}

// a branch right after these is the backend's deliberate always-taken jump
bool SetsKnownFlags(const Compiled& compiled, std::int32_t prev) {
    if (prev < 0) {
        return false;
    }
    const AsmItem& item = compiled.as.items[static_cast<std::size_t>(prev)];
    Mode mode = EncodedMode(compiled.as, item);
    if (IsLoad(item.mn) && mode == Mode::Imm) {
        return true;
    }
    return item.mn == Mn::CLC || item.mn == Mn::SEC || item.mn == Mn::CLV;
}

Finding Make(const Compiled& compiled, const SiteMap& map, const SiteView& v, Pattern pattern,
             std::uint64_t held, std::uint64_t out_of, std::uint64_t saved,
             const std::string& reason) {
    Finding f;
    f.pattern = pattern;
    f.address = v.address;
    f.count = v.s->count;
    f.held = held;
    f.out_of = out_of;
    f.saved = saved;
    f.reason = reason;
    f.function = SiteFunction(compiled, map, v.address);
    f.source = SiteSource(compiled, map, v.address);
    f.text = SiteText(compiled, map, v.address);
    std::string prev = ItemShape(compiled, map.at[v.address].prev);
    f.key = std::string(PatternName(pattern)) + ": " + prev + " ; " + ShapeText(v.mn, v.mode) +
            " (" + reason + ")";
    return f;
}

void FindBranch(const Compiled& compiled, const SiteMap& map, const SiteView& v,
                const FindOptions& options, std::vector<Finding>& out) {
    const SiteStats& s = *v.s;
    bool never = Holds(s.count - s.taken, s.count, options);
    bool always = Holds(s.taken, s.count, options);
    if (!never && !always) {
        return;
    }
    std::uint64_t held = never ? s.count - s.taken : s.taken;
    if (Holds(s.after_compare, s.count, options)) {
        // the compare goes too; a never-taken branch goes, an always-taken one becomes a JMP
        std::uint64_t compare_cycles = 2;
        std::uint32_t at = static_cast<std::uint32_t>(s.compare_site - compiled.origin);
        if (s.compare_site >= compiled.origin && at < compiled.image.size()) {
            compare_cycles = OpcodeInfoFor(compiled.image[at]).cycles;
        }
        std::uint64_t per = compare_cycles + (never ? 2 : 0);
        Finding f =
            Make(compiled, map, v, Pattern::kCompareOneWay, held, s.count, held * per,
                 never ? "compare and branch: never taken" : "compare and branch: always taken");
        f.text = SiteText(compiled, map, s.compare_site) + " / " + f.text;
        out.push_back(f);
        return;
    }
    if (SetsKnownFlags(compiled, map.at[v.address].prev)) {
        return;
    }
    // never taken: the branch goes (2); always taken: laid out to fall through (1)
    out.push_back(Make(compiled, map, v, Pattern::kBranchOneWay, held, s.count,
                       held * (never ? 2 : 1),
                       never ? "branch never taken" : "branch always taken"));
}

// the cycles of every instruction under the same source note as the site
std::uint64_t NoteCycles(const SiteMap& map, const Profiler& p, std::uint16_t address,
                         std::uint32_t lo, std::uint32_t hi) {
    std::int32_t note = map.at[address].note;
    std::uint64_t total = 0;
    for (std::uint32_t a = lo; a < hi; a++) {
        if (map.at[a].note == note && map.at[a].item >= 0) {
            total += p.sites[a].cycles;
        }
    }
    return total;
}

bool IsStrided(const Compiled& compiled, const SiteMap& map, const Profiler& p,
               const FindOptions& options, std::uint32_t a) {
    const SiteStats& s = p.sites[a];
    Mn mn = OpcodeInfoFor(CompiledByte(compiled, static_cast<std::uint16_t>(a))).mn;
    if (!IsStoreMn(mn) || map.at[a].item < 0 || map.at[a].note < 0 || s.count < options.min_count ||
        s.count < 3 || s.last_step == 0 || s.volatile_count > 0) {
        return false;
    }
    double steps = static_cast<double>(s.count - 2);
    return static_cast<double>(s.step_repeats) >= options.strided_fraction * steps;
}

// per source line: its stores that step by a constant against the whole line's work, if
// each such byte were kept and stepped instead by a 16-bit add (CLC LDA ADC STA LDA ADC STA)
void FindStrided(const Compiled& compiled, const SiteMap& map, const Profiler& p,
                 const FindOptions& options, std::vector<Finding>& out) {
    std::uint32_t lo = compiled.origin;
    std::uint32_t hi = compiled.origin + static_cast<std::uint32_t>(compiled.image.size());
    std::vector<std::uint32_t> strided;
    for (std::uint32_t a = lo; a < hi && a < 0x10000; a++) {
        if (IsStrided(compiled, map, p, options, a)) {
            strided.push_back(a);
        }
    }
    std::vector<std::int32_t> done_notes;
    for (std::uint32_t a : strided) {
        std::int32_t note = map.at[a].note;
        if (std::find(done_notes.begin(), done_notes.end(), note) != done_notes.end()) {
            continue;
        }
        done_notes.push_back(note);
        std::uint64_t stores = 0;
        for (std::uint32_t b : strided) {
            stores += map.at[b].note == note ? 1 : 0;
        }
        const SiteStats& s = p.sites[a];
        std::uint64_t block = NoteCycles(map, p, static_cast<std::uint16_t>(a), lo, hi);
        std::uint64_t replacement = s.count * 18 * stores;
        if (block <= replacement) {
            continue;
        }
        const OpcodeInfo& info = OpcodeInfoFor(compiled.image[a - lo]);
        SiteView v;
        v.address = static_cast<std::uint16_t>(a);
        v.s = &s;
        v.mn = info.mn;
        v.mode = info.mode;
        int step = s.last_step < 128 ? s.last_step : s.last_step - 256;
        std::string reason = "the stored byte steps by " + std::string(step >= 0 ? "+" : "") +
                             std::to_string(step) + " each time (" + std::to_string(stores) +
                             (stores == 1 ? " such byte" : " such bytes") +
                             std::string(" on this line): keep them and add");
        Finding f = Make(compiled, map, v, Pattern::kStrided, s.step_repeats, s.count - 2,
                         block - replacement, reason);
        f.key = std::string(PatternName(Pattern::kStrided)) + ": " +
                ItemShape(compiled, map.at[a].prev) + " ; " + ShapeText(info.mn, info.mode) +
                " (steps by a constant)";
        f.estimate = true;
        out.push_back(f);
    }
}

void FindAt(const Compiled& compiled, const SiteMap& map, const SiteView& v,
            const FindOptions& options, std::vector<Finding>& out) {
    const SiteStats& s = *v.s;
    std::uint64_t judged = s.count - s.volatile_count;
    if (s.volatile_count > 0) {
        return;
    }
    if (IsFlagOp(v.mn) && Holds(s.same, judged, options)) {
        out.push_back(Make(compiled, map, v, Pattern::kFlagAlready, s.same, judged,
                           s.same * v.avg_cycles, FlagReason(v.mn)));
    } else if (IsLoad(v.mn) && Holds(s.same, judged, options)) {
        // the flags it sets may still be needed: CMP #0 or a reordering keeps them
        bool flags_read = s.flag_ends > s.flag_dead;
        out.push_back(Make(compiled, map, v, Pattern::kLoadAlready, s.same, judged,
                           s.same * v.avg_cycles,
                           flags_read ? "register already held it (its N/Z are read)"
                                      : "register already held it"));
    } else if (IsTransfer(v.mn) && Holds(s.same, judged, options)) {
        out.push_back(Make(compiled, map, v, Pattern::kTransferAlready, s.same, judged,
                           s.same * v.avg_cycles, "target already held it"));
    } else if (IsStoreMn(v.mn) && Holds(s.same, judged, options)) {
        out.push_back(Make(compiled, map, v, Pattern::kStoreAlready, s.same, judged,
                           s.same * v.avg_cycles, "memory already held the value"));
    }
    if (IsStoreMn(v.mn) && Holds(s.store_dead, s.store_ends, options)) {
        out.push_back(Make(compiled, map, v, Pattern::kStoreDead, s.store_dead, s.store_ends,
                           s.store_dead * v.avg_cycles, "overwritten before any read"));
    }
    if (OnlyRegister(v.mn, v.mode) && Holds(s.reg_dead, s.reg_ends, options) &&
        s.flag_dead == s.flag_ends) {
        out.push_back(Make(compiled, map, v, Pattern::kDeadValue, s.reg_dead, s.reg_ends,
                           s.reg_dead * v.avg_cycles, "nothing reads the value or its flags"));
    }
    if (OnlyFlags(v.mn) && Holds(s.flag_dead, s.flag_ends, options)) {
        out.push_back(Make(compiled, map, v, Pattern::kFlagsUnread, s.flag_dead, s.flag_ends,
                           s.flag_dead * v.avg_cycles, "no flag it sets is read"));
    }
    if (IsBranch(v.mn)) {
        FindBranch(compiled, map, v, options, out);
    }
}

} // namespace

const char* PatternName(Pattern pattern) {
    switch (pattern) {
    case Pattern::kFlagAlready:
        return "flag already set";
    case Pattern::kLoadAlready:
        return "load already held";
    case Pattern::kTransferAlready:
        return "transfer already held";
    case Pattern::kStoreAlready:
        return "store already held";
    case Pattern::kStoreDead:
        return "dead store";
    case Pattern::kCompareOneWay:
        return "compare one way";
    case Pattern::kBranchOneWay:
        return "branch one way";
    case Pattern::kFlagsUnread:
        return "flags unread";
    case Pattern::kStrided:
        return "strided value";
    case Pattern::kDeadValue:
        return "dead value";
    default:
        return "?";
    }
}

std::vector<Finding> FindGaps(const Compiled& compiled, const SiteMap& map, const Profiler& p,
                              const FindOptions& options) {
    std::vector<Finding> out;
    std::uint32_t lo = compiled.origin;
    std::uint32_t hi = compiled.origin + static_cast<std::uint32_t>(compiled.image.size());
    for (std::uint32_t a = lo; a < hi && a < 0x10000; a++) {
        const SiteStats& s = p.sites[a];
        if (s.count == 0 || map.at[a].item < 0) {
            continue;
        }
        const OpcodeInfo& info = OpcodeInfoFor(compiled.image[a - lo]);
        SiteView v;
        v.address = static_cast<std::uint16_t>(a);
        v.s = &s;
        v.mn = info.mn;
        v.mode = info.mode;
        v.avg_cycles = s.cycles / s.count;
        FindAt(compiled, map, v, options, out);
    }
    FindStrided(compiled, map, p, options, out);
    std::stable_sort(out.begin(), out.end(),
                     [](const Finding& x, const Finding& y) { return x.saved > y.saved; });
    return out;
}

std::uint64_t TotalSaved(const std::vector<Finding>& findings) {
    std::map<std::uint16_t, std::uint64_t> best;
    for (const Finding& f : findings) {
        std::uint64_t& b = best[f.address];
        b = f.saved > b ? f.saved : b;
    }
    std::uint64_t total = 0;
    for (const auto& entry : best) {
        total += entry.second;
    }
    return total;
}

} // namespace tisple
