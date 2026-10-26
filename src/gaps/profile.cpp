#include "gaps/profile.hpp"

#include "superopt/exec.hpp"

#include <bit>

namespace tisple {

namespace {

constexpr std::uint8_t kFlagBits[4] = {kFlagN, kFlagZ, kFlagC, kFlagV};
constexpr std::uint8_t kAllFlags = kFlagN | kFlagZ | kFlagC | kFlagV;

bool IsStore(Mn mn) {
    return mn == Mn::STA || mn == Mn::STX || mn == Mn::STY;
}

bool IsCompare(Mn mn) {
    return mn == Mn::CMP || mn == Mn::CPX || mn == Mn::CPY || mn == Mn::BIT;
}

bool HasMemoryOperand(Mode mode) {
    switch (mode) {
    case Mode::Zp:
    case Mode::ZpX:
    case Mode::ZpY:
    case Mode::Abs:
    case Mode::AbsX:
    case Mode::AbsY:
    case Mode::IndX:
    case Mode::IndY:
        return true;
    default:
        return false;
    }
}

std::uint16_t Word(const Machine& m, std::uint16_t at) {
    return static_cast<std::uint16_t>(m.mem[at] | (m.mem[static_cast<std::uint16_t>(at + 1)] << 8));
}

// a pointer in zero page: the high byte wraps inside page zero
std::uint16_t ZpWord(const Machine& m, std::uint8_t at) {
    return static_cast<std::uint16_t>(m.mem[at] | (m.mem[static_cast<std::uint8_t>(at + 1)] << 8));
}

// the data address of the instruction about to run at pc
std::uint16_t EffectiveAddress(const Machine& m, const Regs& r, Mode mode) {
    std::uint8_t zp = m.mem[static_cast<std::uint16_t>(r.pc + 1)];
    std::uint16_t abs = Word(m, static_cast<std::uint16_t>(r.pc + 1));
    switch (mode) {
    case Mode::Zp:
        return zp;
    case Mode::ZpX:
        return static_cast<std::uint8_t>(zp + r.x);
    case Mode::ZpY:
        return static_cast<std::uint8_t>(zp + r.y);
    case Mode::Abs:
        return abs;
    case Mode::AbsX:
        return static_cast<std::uint16_t>(abs + r.x);
    case Mode::AbsY:
        return static_cast<std::uint16_t>(abs + r.y);
    case Mode::IndX:
        return ZpWord(m, static_cast<std::uint8_t>(zp + r.x));
    case Mode::IndY:
        return static_cast<std::uint16_t>(ZpWord(m, zp) + r.y);
    default:
        return 0;
    }
}

void FinishEvent(Profiler& p, FlagEvent& e) {
    if (e.site < 0) {
        return;
    }
    SiteStats& s = p.sites[static_cast<std::size_t>(e.site)];
    s.flag_ends++;
    if (!e.read) {
        s.flag_dead++;
    }
    e.site = -1;
}

void ReadFlags(Profiler& p, std::uint8_t mask) {
    for (int f = 0; f < 4; f++) {
        if ((mask & kFlagBits[f]) != 0 && p.flag_owner[f] >= 0) {
            p.events[p.flag_owner[f]].read = true;
        }
    }
}

void WriteFlags(Profiler& p, std::int32_t site, std::uint8_t mask) {
    if (mask == 0) {
        return;
    }
    for (int f = 0; f < 4; f++) {
        if ((mask & kFlagBits[f]) == 0 || p.flag_owner[f] < 0) {
            continue;
        }
        FlagEvent& e = p.events[p.flag_owner[f]];
        e.refs--;
        if (e.refs == 0) {
            FinishEvent(p, e);
        }
        p.flag_owner[f] = -1;
    }
    std::int8_t slot = 0;
    while (p.events[slot].refs != 0) {
        slot++;
    }
    FlagEvent& e = p.events[slot];
    e.site = site;
    e.refs = static_cast<std::uint8_t>(std::popcount(static_cast<unsigned>(mask)));
    e.read = false;
    for (int f = 0; f < 4; f++) {
        if ((mask & kFlagBits[f]) != 0) {
            p.flag_owner[f] = slot;
        }
    }
}

// the text and graphics pages: the video scanner reads them, never through the cpu
bool IsDisplayMemory(std::uint16_t address) {
    return (address >= 0x0400 && address < 0x0C00) || (address >= 0x2000 && address < 0x6000);
}

// every byte the instruction read or wrote: last writers and whether they were read
void TrackBytes(Profiler& p, const PendingIns& in, Mn mn) {
    bool store = IsStore(mn);
    // the opcode fetch is first; code bytes are never a store's target anyway
    for (const BusCycle& c : p.bus) {
        if (c.addr >= 0xC000) {
            continue;
        }
        if (!c.is_write) {
            // an indexed store's dummy read of its own target is not a use
            if (!(store && in.has_ea && c.addr == in.ea)) {
                p.byte_read[c.addr] = 1;
            }
            continue;
        }
        std::int32_t w = p.writer[c.addr];
        if (w >= 0) {
            SiteStats& s = p.sites[static_cast<std::size_t>(w)];
            s.store_ends++;
            if (p.byte_read[c.addr] == 0) {
                s.store_dead++;
            }
        }
        bool judged = store && !IsDisplayMemory(c.addr);
        p.writer[c.addr] = judged ? static_cast<std::int32_t>(in.pc) : -1;
        p.byte_read[c.addr] = 0;
    }
}

// registers read, then written: the last writer of each and whether anything read it
void TrackRegisters(Profiler& p, std::int32_t site, Mn mn, Mode mode) {
    LocEffects e = EffectsOf(mn, mode);
    const std::uint8_t bits[3] = {kLocA, kLocX, kLocY};
    for (int r = 0; r < 3; r++) {
        if ((e.reads & bits[r]) != 0) {
            p.reg_read[r] = true;
        }
    }
    for (int r = 0; r < 3; r++) {
        if ((e.writes & bits[r]) == 0) {
            continue;
        }
        if (p.reg_owner[r] >= 0) {
            SiteStats& s = p.sites[static_cast<std::size_t>(p.reg_owner[r])];
            s.reg_ends++;
            if (!p.reg_read[r]) {
                s.reg_dead++;
            }
        }
        p.reg_owner[r] = site;
        p.reg_read[r] = false;
    }
}

void NoteEntryFacts(SiteStats& s, const PendingIns& in, bool reads_memory) {
    const Regs& r = in.before;
    s.p_and &= r.p;
    s.p_or |= r.p;
    s.a_min = r.a < s.a_min ? r.a : s.a_min;
    s.a_max = r.a > s.a_max ? r.a : s.a_max;
    s.x_min = r.x < s.x_min ? r.x : s.x_min;
    s.x_max = r.x > s.x_max ? r.x : s.x_max;
    s.y_min = r.y < s.y_min ? r.y : s.y_min;
    s.y_max = r.y > s.y_max ? r.y : s.y_max;
    if (reads_memory && in.has_ea && !in.volatile_ea) {
        s.m_min = in.old_value < s.m_min ? in.old_value : s.m_min;
        s.m_max = in.old_value > s.m_max ? in.old_value : s.m_max;
    }
}

void NoteStoredValue(SiteStats& s, std::uint8_t value) {
    if (s.count > 1) {
        std::uint8_t step = static_cast<std::uint8_t>(value - s.last_value);
        if (s.count > 2 && step == s.last_step) {
            s.step_repeats++;
        }
        s.last_step = step;
    }
    s.last_value = value;
}

// did the instruction leave its target as it found it
bool AlreadyHeld(const PendingIns& in, Mn mn, const Regs& b, const Regs& a) {
    switch (mn) {
    case Mn::CLC:
        return (b.p & kFlagC) == 0;
    case Mn::SEC:
        return (b.p & kFlagC) != 0;
    case Mn::CLV:
        return (b.p & kFlagV) == 0;
    case Mn::CLD:
        return (b.p & kFlagD) == 0;
    case Mn::SED:
        return (b.p & kFlagD) != 0;
    case Mn::CLI:
        return (b.p & kFlagI) == 0;
    case Mn::SEI:
        return (b.p & kFlagI) != 0;
    case Mn::LDA:
    case Mn::TXA:
    case Mn::TYA:
        return !in.volatile_ea && b.a == a.a;
    case Mn::LDX:
    case Mn::TAX:
    case Mn::TSX:
        return !in.volatile_ea && b.x == a.x;
    case Mn::LDY:
    case Mn::TAY:
        return !in.volatile_ea && b.y == a.y;
    case Mn::TXS:
        return b.s == a.s;
    case Mn::STA:
        return !in.volatile_ea && in.old_value == b.a;
    case Mn::STX:
        return !in.volatile_ea && in.old_value == b.x;
    case Mn::STY:
        return !in.volatile_ea && in.old_value == b.y;
    default:
        return false;
    }
}

void Judge(Profiler& p, const Regs& after) {
    const PendingIns& in = p.pending;
    const OpcodeInfo& info = OpcodeInfoFor(in.opcode);
    Mn mn = info.mn;
    SiteStats& s = p.sites[in.pc];
    s.count++;
    s.cycles += p.bus.size();
    p.instructions++;
    p.cycles += p.bus.size();
    NoteEntryFacts(s, in, !IsStore(mn));
    if (in.volatile_ea) {
        s.volatile_count++;
    }
    if (AlreadyHeld(in, mn, in.before, after)) {
        s.same++;
    }
    if (mn == Mn::STA || mn == Mn::STX || mn == Mn::STY) {
        std::uint8_t value = mn == Mn::STA ? in.before.a : mn == Mn::STX ? in.before.x : in.before.y;
        NoteStoredValue(s, value);
    }
    if (IsBranch(mn)) {
        if (after.pc != static_cast<std::uint16_t>(in.pc + 2)) {
            s.taken++;
        }
        if (IsCompare(p.last_mn)) {
            s.after_compare++;
            s.compare_site = p.last_pc;
        }
    }
    ReadFlags(p, FlagsRead(mn));
    WriteFlags(p, static_cast<std::int32_t>(in.pc), FlagsWritten(mn));
    TrackBytes(p, in, mn);
    TrackRegisters(p, static_cast<std::int32_t>(in.pc), mn, info.mode);
    p.last_pc = in.pc;
    p.last_mn = mn;
}

void NotePending(Profiler& p, const Machine& m) {
    PendingIns& in = p.pending;
    in.valid = true;
    in.before = GetRegs(m);
    in.pc = in.before.pc;
    in.opcode = m.mem[in.pc];
    const OpcodeInfo& info = OpcodeInfoFor(in.opcode);
    in.has_ea = HasMemoryOperand(info.mode);
    in.ea = in.has_ea ? EffectiveAddress(m, in.before, info.mode) : 0;
    in.volatile_ea = in.has_ea && in.ea >= 0xC000;
    in.old_value = in.has_ea && !in.volatile_ea ? m.mem[in.ea] : 0;
}

} // namespace

std::uint8_t FlagsRead(Mn mn) {
    switch (mn) {
    case Mn::BCC:
    case Mn::BCS:
    case Mn::ADC:
    case Mn::SBC:
    case Mn::ROL:
    case Mn::ROR:
        return kFlagC;
    case Mn::BEQ:
    case Mn::BNE:
        return kFlagZ;
    case Mn::BMI:
    case Mn::BPL:
        return kFlagN;
    case Mn::BVC:
    case Mn::BVS:
        return kFlagV;
    case Mn::PHP:
    case Mn::BRK:
        return kAllFlags;
    default:
        return 0;
    }
}

std::uint8_t FlagsWritten(Mn mn) {
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
    case Mn::INC:
    case Mn::DEC:
    case Mn::AND:
    case Mn::ORA:
    case Mn::EOR:
    case Mn::PLA:
        return kFlagN | kFlagZ;
    case Mn::ADC:
    case Mn::SBC:
    case Mn::PLP:
    case Mn::RTI:
        return kAllFlags;
    case Mn::CMP:
    case Mn::CPX:
    case Mn::CPY:
    case Mn::ASL:
    case Mn::LSR:
    case Mn::ROL:
    case Mn::ROR:
        return kFlagN | kFlagZ | kFlagC;
    case Mn::BIT:
        return kFlagN | kFlagZ | kFlagV;
    case Mn::CLC:
    case Mn::SEC:
        return kFlagC;
    case Mn::CLV:
        return kFlagV;
    default:
        return 0;
    }
}

void ProfileTrace(void* ctx, const Machine& m) {
    Profiler& p = *static_cast<Profiler*>(ctx);
    if (p.pending.valid) {
        Judge(p, GetRegs(m));
    }
    p.bus.clear();
    NotePending(p, m);
}

void FinishProfile(Profiler& p, const Machine& m) {
    if (p.pending.valid) {
        Judge(p, GetRegs(m));
    }
    p.pending.valid = false;
    p.bus.clear();
}

void ResetProfileTracking(Profiler& p) {
    p.pending.valid = false;
    p.bus.clear();
    p.last_mn = Mn::NOP;
    p.writer.assign(65536, -1);
    p.byte_read.assign(65536, 0);
    for (FlagEvent& e : p.events) {
        e = FlagEvent();
    }
    for (std::int8_t& owner : p.flag_owner) {
        owner = -1;
    }
    for (int r = 0; r < 3; r++) {
        p.reg_owner[r] = -1;
        p.reg_read[r] = false;
    }
}

} // namespace tisple
