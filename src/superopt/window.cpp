#include "superopt/window.hpp"

#include <algorithm>

namespace tisple {

namespace {

constexpr int kScanLimit = 64;

bool IsDirectMode(Mode mode) {
    return mode == Mode::Zp || mode == Mode::Abs;
}

bool IsMemoryMode(Mode mode) {
    return mode != Mode::Imp && mode != Mode::Acc && mode != Mode::Imm && mode != Mode::Rel;
}

bool EndsScan(Mn mn) {
    return IsBranch(mn) || mn == Mn::JMP || mn == Mn::JSR || mn == Mn::RTS || mn == Mn::RTI ||
           mn == Mn::BRK;
}

CellIndex IndexOf(Mode mode) {
    switch (mode) {
    case Mode::ZpX:
    case Mode::AbsX:
    case Mode::IndX:
        return CellIndex::X;
    case Mode::ZpY:
    case Mode::AbsY:
    case Mode::IndY:
        return CellIndex::Y;
    default:
        return CellIndex::None;
    }
}

std::int8_t FindCell(const std::vector<Cell>& cells, Mode mode, std::uint16_t operand) {
    bool direct = IsDirectMode(mode);
    for (std::size_t i = 0; i < cells.size(); i++) {
        const Cell& c = cells[i];
        bool same_kind = direct ? IsDirectCell(c) : c.mode == mode;
        if (same_kind && c.operand == operand) {
            return static_cast<std::int8_t>(i);
        }
    }
    return -1;
}

struct Scan {
    std::uint8_t open = kLocRegs | kLocFlags; // not yet known read or written
    std::uint8_t live = 0;
    std::vector<std::uint16_t> open_bytes; // direct cells the window writes
    std::vector<std::uint16_t> live_bytes;
};

void ByteRead(Scan& s, std::uint16_t address) {
    auto it = std::find(s.open_bytes.begin(), s.open_bytes.end(), address);
    if (it != s.open_bytes.end()) {
        s.live_bytes.push_back(address);
        s.open_bytes.erase(it);
    }
}

void ByteWritten(Scan& s, std::uint16_t address) {
    auto it = std::find(s.open_bytes.begin(), s.open_bytes.end(), address);
    if (it != s.open_bytes.end()) {
        s.open_bytes.erase(it);
    }
}

void AllLive(Scan& s) {
    s.live |= s.open;
    s.open = 0;
    for (std::uint16_t b : s.open_bytes) {
        s.live_bytes.push_back(b);
    }
    s.open_bytes.clear();
}

// one instruction after the window; false when the scan stops here
bool ScanOne(Scan& s, const RawIns& ins) {
    if (ins.mn == Mn::JMP || ins.mn == Mn::JSR || ins.mn == Mn::RTS || ins.mn == Mn::RTI ||
        ins.mn == Mn::BRK) {
        AllLive(s);
        return false;
    }
    LocEffects e = EffectsOf(ins.mn, ins.mode);
    s.live |= s.open & e.reads;
    s.open &= static_cast<std::uint8_t>(~e.reads);
    if (e.mem_read) {
        if (IsDirectMode(ins.mode)) {
            ByteRead(s, ins.operand);
        } else {
            // through an index or a pointer: could be any byte
            for (std::uint16_t b : s.open_bytes) {
                s.live_bytes.push_back(b);
            }
            s.open_bytes.clear();
        }
    }
    if (ins.mode == Mode::IndX) {
        // the pointer itself is found through X
        AllLive(s);
        return false;
    }
    if (ins.mode == Mode::IndY) {
        ByteRead(s, ins.operand);
        ByteRead(s, static_cast<std::uint8_t>(ins.operand + 1));
    }
    if (e.mem_write && IsDirectMode(ins.mode)) {
        ByteWritten(s, ins.operand);
    }
    s.open &= static_cast<std::uint8_t>(~e.writes);
    if (IsBranch(ins.mn)) {
        AllLive(s);
        return false;
    }
    return true;
}

void AddImm(std::vector<std::uint8_t>& imms, unsigned v) {
    std::uint8_t b = static_cast<std::uint8_t>(v);
    if (std::find(imms.begin(), imms.end(), b) == imms.end() && imms.size() < 10) {
        imms.push_back(b);
    }
}

bool IsIo(const RawIns& ins) {
    return IsMemoryMode(ins.mode) && ins.mode != Mode::IndX && ins.mode != Mode::IndY &&
           ins.operand >= 0xC000;
}

RawIns RawFromItem(const Compiled& compiled, const AsmItem& item) {
    RawIns raw;
    raw.mn = item.mn;
    raw.mode = EncodedMode(compiled.as, item);
    raw.address = item.address;
    std::size_t at = item.address - compiled.origin;
    int size = ModeSize(raw.mode);
    if (size >= 2 && at + 1 < compiled.image.size()) {
        raw.operand = compiled.image[at + 1];
    }
    if (size == 3 && at + 2 < compiled.image.size()) {
        raw.operand = static_cast<std::uint16_t>(raw.operand | (compiled.image[at + 2] << 8));
    }
    return raw;
}

} // namespace

bool MakeWindow(const std::vector<RawIns>& code, const std::vector<RawIns>& after, Window& out,
                std::string& why) {
    out = Window();
    if (code.empty()) {
        why = "empty";
        return false;
    }
    out.address = code[0].address;
    AddImm(out.imms, 0);
    AddImm(out.imms, 1);
    AddImm(out.imms, 0xFF);
    std::uint8_t written = 0;
    std::uint8_t index_moved = 0; // kLocX / kLocY written so far
    bool any_write = false;
    bool any_pseudo = false;
    std::vector<std::uint16_t> pointer_bytes;
    for (const RawIns& raw : code) {
        if (!IsModelled(raw.mn) || raw.mode == Mode::Ind || raw.mode == Mode::Rel) {
            why = std::string("not modelled: ") + MnName(raw.mn);
            return false;
        }
        if (IsIo(raw)) {
            why = "i/o or rom operand";
            return false;
        }
        SoIns ins;
        ins.mn = raw.mn;
        ins.mode = raw.mode;
        LocEffects e = EffectsOf(raw.mn, raw.mode);
        if (raw.mode == Mode::Imm) {
            ins.imm = static_cast<std::uint8_t>(raw.operand);
            AddImm(out.imms, ins.imm);
        } else if (IsMemoryMode(raw.mode)) {
            CellIndex index = IndexOf(raw.mode);
            if ((index == CellIndex::X && (index_moved & kLocX) != 0) ||
                (index == CellIndex::Y && (index_moved & kLocY) != 0)) {
                why = "an index register moves before an indexed access";
                return false;
            }
            std::int8_t cell = FindCell(out.cells, raw.mode, raw.operand);
            if (cell < 0) {
                if (out.cells.size() >= kMaxCells) {
                    why = "too many memory operands";
                    return false;
                }
                Cell c;
                c.mode = IsDirectMode(raw.mode) ? (raw.operand < 0x100 ? Mode::Zp : Mode::Abs)
                                                : raw.mode;
                c.operand = raw.operand;
                c.index = index;
                out.cells.push_back(c);
                cell = static_cast<std::int8_t>(out.cells.size() - 1);
            }
            ins.cell = cell;
            Cell& c = out.cells[static_cast<std::size_t>(cell)];
            if (e.mem_write) {
                c.writable = true;
                any_write = true;
            }
            if (!IsDirectCell(c)) {
                any_pseudo = true;
            }
            if (raw.mode == Mode::IndX || raw.mode == Mode::IndY) {
                pointer_bytes.push_back(static_cast<std::uint8_t>(raw.operand));
                pointer_bytes.push_back(static_cast<std::uint8_t>(raw.operand + 1));
            }
        }
        written |= e.writes;
        index_moved |= e.writes & (kLocX | kLocY);
        out.code.push_back(ins);
        out.cycles += InsCycles(ins);
        out.bytes += InsBytes(ins);
    }
    for (const Cell& c : out.cells) {
        if (IsDirectCell(c) &&
            std::find(pointer_bytes.begin(), pointer_bytes.end(), c.operand) != pointer_bytes.end()) {
            why = "a pointer byte is also an operand";
            return false;
        }
    }
    out.assumes_no_alias = any_write && any_pseudo && out.cells.size() > 1;

    Scan scan;
    for (const Cell& c : out.cells) {
        if (IsDirectCell(c) && c.writable) {
            scan.open_bytes.push_back(c.operand);
        }
    }
    bool stopped = false;
    for (std::size_t i = 0; i < after.size() && i < kScanLimit; i++) {
        if (!ScanOne(scan, after[i])) {
            stopped = true;
            break;
        }
    }
    if (!stopped) {
        AllLive(scan);
    }
    out.live = scan.live;
    for (Cell& c : out.cells) {
        // only a direct byte the window writes and the scan overwrites unread is dead
        c.live = !(IsDirectCell(c) && c.writable &&
                   std::find(scan.live_bytes.begin(), scan.live_bytes.end(), c.operand) ==
                       scan.live_bytes.end());
    }
    out.fixed = static_cast<std::uint8_t>(out.live & ~written);
    return true;
}

std::vector<RawIns> FollowingIns(const Compiled& compiled, std::size_t span, std::size_t item) {
    std::vector<RawIns> out;
    const FunctionSpan& s = compiled.spans[span];
    for (std::size_t i = item + 1; i < s.end && out.size() < kScanLimit; i++) {
        const AsmItem& it = compiled.as.items[i];
        if (it.kind == ItemKind::Note) {
            continue;
        }
        if (it.kind != ItemKind::Ins) {
            break;
        }
        RawIns raw = RawFromItem(compiled, it);
        if (it.long_branch) {
            raw.mn = Mn::JMP; // a branch over a jump: both ways leave
        }
        out.push_back(raw);
        if (EndsScan(raw.mn)) {
            break;
        }
    }
    return out;
}

std::vector<RawRun> StraightRuns(const Compiled& compiled) {
    std::vector<RawRun> runs;
    for (std::size_t s = 0; s < compiled.spans.size(); s++) {
        const FunctionSpan& span = compiled.spans[s];
        RawRun run;
        run.span = s;
        auto flush = [&]() {
            if (run.ins.size() >= 2) {
                runs.push_back(run);
            }
            run.ins.clear();
            run.items.clear();
        };
        for (std::size_t i = span.first; i < span.end; i++) {
            const AsmItem& item = compiled.as.items[i];
            if (item.kind == ItemKind::Note) {
                continue;
            }
            if (item.kind != ItemKind::Ins) {
                flush();
                continue;
            }
            RawIns raw = RawFromItem(compiled, item);
            if (!IsModelled(raw.mn) || item.long_branch || IsIo(raw)) {
                flush();
                continue;
            }
            run.ins.push_back(raw);
            run.items.push_back(i);
        }
        flush();
    }
    return runs;
}

std::string WindowKey(const Window& w) {
    std::string key = CodeText(w.code, w.cells.data());
    key += " | live " + std::to_string(w.live) + " ";
    for (const Cell& c : w.cells) {
        key += c.live ? 'L' : 'd';
    }
    return key;
}

} // namespace tisple
