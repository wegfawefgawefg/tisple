// The overlay groups' stubs, their one enter routine and the copy (overlay.hpp: RouteOverlays),
// and what a load costs.

#include "backend/aux_code.hpp"
#include "backend/overlay.hpp"

namespace tisple {

namespace {

constexpr std::uint16_t kRamrdOn = 0xC003;
constexpr std::uint16_t kRamrdOff = 0xC002;

// lib/disk's reader (disk.tsp): its entry and the next sector's, its arguments in auxmem.from
// (the table's record) and auxmem.to (where to), X the memory (0 main) and drive 2's bit
constexpr std::uint16_t kReaderBegin = 0x1700;
constexpr std::uint16_t kReaderNext = 0x1703;
constexpr std::uint16_t kReaderFrom = 0x00FA;
constexpr std::uint16_t kReaderTo = 0x00FC;
constexpr std::uint8_t kReaderMore = 1;

// a group read from the second disk has this in ov.pages, which no image in aux memory has
constexpr std::uint8_t kFromDisk = 0xFF;

// a JSR, a JMP to a label, or a branch: control goes to the operand
bool IsTransfer(const AsmItem& item) {
    if (item.kind != ItemKind::Ins) {
        return false;
    }
    return item.mn == Mn::JSR || (item.mn == Mn::JMP && item.mode == Mode::Abs) ||
           item.mode == Mode::Rel;
}

void Ins(std::vector<AsmItem>& out, Mn mn, Mode mode, AsmValue value) {
    out.push_back(MakeIns(mn, mode, value));
}

void Imp(std::vector<AsmItem>& out, std::initializer_list<Mn> mns) {
    for (Mn mn : mns) {
        out.push_back(MakeIns(mn, Mode::Imp));
    }
}

// a JMP's three bytes, as data: a template of a stub
void JmpBytes(std::vector<AsmItem>& out, std::int32_t to) {
    out.push_back(MakeByte(Num(0x4C)));
    out.push_back(MakeByte(SymValue(to, 0, Part::Low)));
    out.push_back(MakeByte(SymValue(to, 0, Part::High)));
}

// group X's image read from the second disk into the region: its record (ov.lo and ov.hi
// point at it) given to the reader, run to the last sector, and on any error (the disk out,
// a bad read) read again until it is all in: the code must be whole
void ReadImage(Backend& be, const OverlayCode& code, std::int32_t lo, std::int32_t hi,
               std::int32_t disk2_bit_sym, std::vector<AsmItem>& out) {
    std::int32_t retry = NewSym(be, "ov.retry", SymRole::Label, -1);
    std::int32_t more = NewSym(be, "ov.more", SymRole::Label, -1);
    std::int32_t got = NewSym(be, "ov.got", SymRole::Label, -1);
    Ins(out, Mn::LDA, Mode::AbsX, SymValue(lo));
    Ins(out, Mn::STA, Mode::Abs, SymValue(be.rt_a));
    Ins(out, Mn::LDA, Mode::AbsX, SymValue(hi));
    Ins(out, Mn::STA, Mode::Abs, SymValue(be.rt_a, 1));
    out.push_back(MakeLabel(retry));
    Ins(out, Mn::LDA, Mode::Abs, SymValue(be.rt_a));
    Ins(out, Mn::STA, Mode::Zp, Num(kReaderFrom));
    Ins(out, Mn::LDA, Mode::Abs, SymValue(be.rt_a, 1));
    Ins(out, Mn::STA, Mode::Zp, Num(kReaderFrom + 1));
    Ins(out, Mn::LDA, Mode::Imm, SymValue(code.region_sym, 0, Part::Low));
    Ins(out, Mn::STA, Mode::Zp, Num(kReaderTo));
    Ins(out, Mn::LDA, Mode::Imm, SymValue(code.region_sym, 0, Part::High));
    Ins(out, Mn::STA, Mode::Zp, Num(kReaderTo + 1));
    // main memory (0) and the drive the second disk is in (the record's $80 asks for it)
    Ins(out, Mn::LDX, Mode::Abs, SymValue(disk2_bit_sym));
    Ins(out, Mn::JSR, Mode::Abs, Num(kReaderBegin));
    out.push_back(MakeLabel(more));
    Ins(out, Mn::CMP, Mode::Imm, Num(kReaderMore));
    Ins(out, Mn::BNE, Mode::Rel, SymValue(got));
    Ins(out, Mn::JSR, Mode::Abs, Num(kReaderNext));
    Ins(out, Mn::JMP, Mode::Abs, SymValue(more));
    out.push_back(MakeLabel(got));
    Ins(out, Mn::CMP, Mode::Imm, Num(0));
    Ins(out, Mn::BNE, Mode::Rel, SymValue(retry));
}

// the copy: X whole pages, then rt_r bytes, from (rt_a) in aux memory to (rt_b)
void EmitCopy(Backend& be, std::int32_t copy, std::vector<AsmItem>& c) {
    std::int32_t rt_a = be.rt_a;
    std::int32_t rt_b = be.rt_b;
    std::int32_t rt_r = be.rt_r;
    std::int32_t page = NewSym(be, "ov.copy.page", SymRole::Label, -1);
    std::int32_t byte = NewSym(be, "ov.copy.byte", SymRole::Label, -1);
    std::int32_t tail = NewSym(be, "ov.copy.tail", SymRole::Label, -1);
    std::int32_t last = NewSym(be, "ov.copy.last", SymRole::Label, -1);
    std::int32_t done = NewSym(be, "ov.copy.done", SymRole::Label, -1);
    c.push_back(MakeLabel(copy));
    Ins(c, Mn::LDY, Mode::Imm, Num(0));
    Ins(c, Mn::CPX, Mode::Imm, Num(0));
    Ins(c, Mn::BEQ, Mode::Rel, SymValue(tail));
    c.push_back(MakeLabel(page));
    Imp(c, {Mn::PHP, Mn::SEI});
    Ins(c, Mn::STA, Mode::Abs, Num(kRamrdOn));
    c.push_back(MakeLabel(byte));
    Ins(c, Mn::LDA, Mode::IndY, SymValue(rt_a));
    Ins(c, Mn::STA, Mode::IndY, SymValue(rt_b));
    Imp(c, {Mn::INY});
    Ins(c, Mn::BNE, Mode::Rel, SymValue(byte));
    Ins(c, Mn::STA, Mode::Abs, Num(kRamrdOff));
    Imp(c, {Mn::PLP});
    Ins(c, Mn::INC, Mode::Abs, SymValue(rt_a, 1));
    Ins(c, Mn::INC, Mode::Abs, SymValue(rt_b, 1));
    Imp(c, {Mn::DEX});
    Ins(c, Mn::BNE, Mode::Rel, SymValue(page));
    c.push_back(MakeLabel(tail));
    Ins(c, Mn::LDY, Mode::Abs, SymValue(rt_r));
    Ins(c, Mn::BEQ, Mode::Rel, SymValue(done));
    Imp(c, {Mn::PHP, Mn::SEI});
    Ins(c, Mn::STA, Mode::Abs, Num(kRamrdOn));
    c.push_back(MakeLabel(last));
    Imp(c, {Mn::DEY});
    Ins(c, Mn::LDA, Mode::IndY, SymValue(rt_a));
    Ins(c, Mn::STA, Mode::IndY, SymValue(rt_b));
    Imp(c, {Mn::TYA});
    Ins(c, Mn::BNE, Mode::Rel, SymValue(last));
    Ins(c, Mn::STA, Mode::Abs, Num(kRamrdOff));
    Imp(c, {Mn::PLP});
    c.push_back(MakeLabel(done));
    Imp(c, {Mn::RTS});
}

} // namespace

void RouteOverlays(Backend& be, const OverlayGroups& groups,
                   const std::vector<std::vector<AsmItem>*>& pieces,
                   const std::vector<std::int32_t>& piece_group, const std::vector<bool>& on_disk,
                   std::int32_t disk2_bit_sym, OverlayCode& code) {
    const Program& program = *be.program;
    std::size_t count = program.functions.size();
    std::size_t n = groups.roots.size();
    code = OverlayCode();
    code.stub.assign(count, -1);
    code.region_sym = NewSym(be, "ov.region", SymRole::Data, -1);
    std::vector<std::int32_t> load(count, -1);
    std::vector<std::int32_t> enter(n, -1);
    std::vector<std::int32_t> record(n, -1);
    bool any_disk = false;
    bool any_aux = false;
    std::size_t roots = 0;
    for (std::size_t g = 0; g < n; g++) {
        std::string k = std::to_string(g);
        enter[g] = NewSym(be, "ov." + k + ".enter", SymRole::Label, -1);
        code.image_sym.push_back(NewSym(be, "ov." + k + ".image", SymRole::Data, -1));
        code.size_sym.push_back(NewSym(be, "ov." + k + ".size", SymRole::Data, -1));
        code.on_disk.push_back(g < on_disk.size() && on_disk[g] && disk2_bit_sym >= 0);
        any_disk = any_disk || code.on_disk[g];
        any_aux = any_aux || !code.on_disk[g];
        if (code.on_disk[g]) {
            record[g] = NewSym(be, "ov." + k + ".record", SymRole::Label, -1);
        }
        for (int i = 0; i < 4; i++) {
            code.rec_sym.push_back(
                NewSym(be, "ov." + k + ".record" + std::to_string(i), SymRole::Data, -1));
        }
        for (std::int32_t r : groups.roots[g]) {
            const std::string& name = program.functions[static_cast<std::size_t>(r)].name;
            code.stub[static_cast<std::size_t>(r)] = NewSym(be, "ov." + name, SymRole::Label, -1);
            load[static_cast<std::size_t>(r)] =
                NewSym(be, "ov." + name + ".load", SymRole::Label, -1);
            roots++;
        }
    }
    std::int32_t copy = NewSym(be, "ov.copy", SymRole::Label, -1);
    std::int32_t shared = NewSym(be, "ov.enter", SymRole::Label, -1);
    std::int32_t off = NewSym(be, "ov.off", SymRole::Label, -1);
    std::int32_t on = NewSym(be, "ov.on", SymRole::Label, -1);
    std::int32_t jumps_off = NewSym(be, "ov.jumps_off", SymRole::Data, -1);
    std::int32_t jumps_on = NewSym(be, "ov.jumps_on", SymRole::Data, -1);
    std::int32_t at = NewSym(be, "ov.at", SymRole::Data, -1);
    std::int32_t lo = NewSym(be, "ov.lo", SymRole::Data, -1);
    std::int32_t hi = NewSym(be, "ov.hi", SymRole::Data, -1);
    std::int32_t pages = NewSym(be, "ov.pages", SymRole::Data, -1);
    std::int32_t rest = NewSym(be, "ov.rest", SymRole::Data, -1);
    std::int32_t from_disk = NewSym(be, "ov.from_disk", SymRole::Label, -1);
    std::int32_t rt_a = be.rt_a;
    std::int32_t rt_b = be.rt_b;
    std::int32_t rt_r = be.rt_r;
    std::vector<AsmItem>& out = code.stubs;
    // the stubs, one JMP a root, three bytes apart in group order: what the root's callers call
    std::int32_t jumps = -1;
    for (std::size_t g = 0; g < n; g++) {
        for (std::int32_t r : groups.roots[g]) {
            std::size_t f = static_cast<std::size_t>(r);
            jumps = jumps < 0 ? code.stub[f] : jumps;
            out.push_back(MakeLabel(code.stub[f]));
            Ins(out, Mn::JMP, Mode::Abs, SymValue(load[f]));
            out.back().far = true; // rewritten through the templates: never a 65C02 BRA
        }
    }
    // each root's load, and each group's way into the one enter routine with its number in A
    for (std::size_t g = 0; g < n; g++) {
        for (std::int32_t r : groups.roots[g]) {
            std::size_t f = static_cast<std::size_t>(r);
            out.push_back(MakeLabel(load[f]));
            Ins(out, Mn::JSR, Mode::Abs, SymValue(enter[g]));
            Ins(out, Mn::JMP, Mode::Abs, SymValue(be.fn_sym[f]));
        }
        out.push_back(MakeLabel(enter[g]));
        Imp(out, {Mn::PHP, Mn::PHA});
        Ins(out, Mn::LDA, Mode::Imm, Num(static_cast<std::int32_t>(g)));
        Ins(out, Mn::JMP, Mode::Abs, SymValue(shared));
    }
    // ov.enter: the call's registers and flags kept, every stub back at its load (the off
    // template over them), group A's image copied or read in, its stubs at its roots (the on
    // template's part of them), the registers and flags back
    out.push_back(MakeLabel(shared));
    Ins(out, Mn::STA, Mode::Abs, SymValue(rt_r));
    Imp(out, {Mn::TXA, Mn::PHA, Mn::TYA, Mn::PHA});
    Ins(out, Mn::LDA, Mode::Abs, SymValue(rt_r));
    Imp(out, {Mn::PHA});
    Ins(out, Mn::LDY, Mode::Imm, Num(static_cast<std::int32_t>(3 * roots)));
    out.push_back(MakeLabel(off));
    Ins(out, Mn::LDA, Mode::AbsY, SymValue(jumps_off, -1));
    Ins(out, Mn::STA, Mode::AbsY, SymValue(jumps, -1));
    Imp(out, {Mn::DEY});
    Ins(out, Mn::BNE, Mode::Rel, SymValue(off));
    // the group's number, kept on the stack for its stubs after the load
    Imp(out, {Mn::PLA, Mn::PHA, Mn::TAX});
    if (any_disk && any_aux) {
        Ins(out, Mn::LDA, Mode::AbsX, SymValue(pages));
        Ins(out, Mn::CMP, Mode::Imm, Num(kFromDisk));
        Ins(out, Mn::BEQ, Mode::Rel, SymValue(from_disk));
    }
    if (any_aux) {
        Ins(out, Mn::LDA, Mode::AbsX, SymValue(lo));
        Ins(out, Mn::STA, Mode::Abs, SymValue(rt_a));
        Ins(out, Mn::LDA, Mode::AbsX, SymValue(hi));
        Ins(out, Mn::STA, Mode::Abs, SymValue(rt_a, 1));
        Ins(out, Mn::LDA, Mode::Imm, SymValue(code.region_sym, 0, Part::Low));
        Ins(out, Mn::STA, Mode::Abs, SymValue(rt_b));
        Ins(out, Mn::LDA, Mode::Imm, SymValue(code.region_sym, 0, Part::High));
        Ins(out, Mn::STA, Mode::Abs, SymValue(rt_b, 1));
        Ins(out, Mn::LDA, Mode::AbsX, SymValue(rest));
        Ins(out, Mn::STA, Mode::Abs, SymValue(rt_r));
        Ins(out, Mn::LDA, Mode::AbsX, SymValue(pages));
        Imp(out, {Mn::TAX});
        Ins(out, Mn::JSR, Mode::Abs, SymValue(copy));
    }
    if (any_disk && any_aux) {
        Ins(out, Mn::JMP, Mode::Abs, SymValue(on));
    }
    if (any_disk) {
        out.push_back(MakeLabel(from_disk));
        ReadImage(be, code, lo, hi, disk2_bit_sym, out);
    }
    out.push_back(MakeLabel(on));
    Imp(out, {Mn::PLA, Mn::TAX});
    Ins(out, Mn::LDY, Mode::AbsX, SymValue(at, 1));
    std::int32_t on_loop = NewSym(be, "ov.on.loop", SymRole::Label, -1);
    out.push_back(MakeLabel(on_loop));
    Ins(out, Mn::LDA, Mode::AbsY, SymValue(jumps_on, -1));
    Ins(out, Mn::STA, Mode::AbsY, SymValue(jumps, -1));
    Imp(out, {Mn::DEY, Mn::TYA});
    Ins(out, Mn::CMP, Mode::AbsX, SymValue(at));
    Ins(out, Mn::BNE, Mode::Rel, SymValue(on_loop));
    Imp(out, {Mn::PLA, Mn::TAY, Mn::PLA, Mn::TAX, Mn::PLA, Mn::PLP, Mn::RTS});
    // the templates: every stub at its load, and at its root
    out.push_back(MakeLabel(jumps_off));
    for (std::size_t g = 0; g < n; g++) {
        for (std::int32_t r : groups.roots[g]) {
            JmpBytes(out, load[static_cast<std::size_t>(r)]);
        }
    }
    out.push_back(MakeLabel(jumps_on));
    for (std::size_t g = 0; g < n; g++) {
        for (std::int32_t r : groups.roots[g]) {
            JmpBytes(out, be.fn_sym[static_cast<std::size_t>(r)]);
        }
    }
    // by group: where its stubs start (3 a root; and where the last ends), its image in aux
    // memory or its disk record, the image's whole pages (kFromDisk: read from the disk) and
    // the bytes after them
    out.push_back(MakeLabel(at));
    std::size_t first = 0;
    for (std::size_t g = 0; g <= n; g++) {
        out.push_back(MakeByte(Num(static_cast<std::int32_t>(3 * first))));
        first += g < n ? groups.roots[g].size() : 0;
    }
    auto image_or_record = [&](std::size_t g, Part part) {
        return code.on_disk[g] ? SymValue(record[g], 0, part)
                               : SymValue(code.image_sym[g], 0, part);
    };
    out.push_back(MakeLabel(lo));
    for (std::size_t g = 0; g < n; g++) {
        out.push_back(MakeByte(image_or_record(g, Part::Low)));
    }
    out.push_back(MakeLabel(hi));
    for (std::size_t g = 0; g < n; g++) {
        out.push_back(MakeByte(image_or_record(g, Part::High)));
    }
    if (any_aux) {
        out.push_back(MakeLabel(pages));
        for (std::size_t g = 0; g < n; g++) {
            out.push_back(MakeByte(code.on_disk[g] ? Num(kFromDisk)
                                                   : SymValue(code.size_sym[g], 0, Part::Low)));
        }
        out.push_back(MakeLabel(rest));
        for (std::size_t g = 0; g < n; g++) {
            out.push_back(
                MakeByte(code.on_disk[g] ? Num(0) : SymValue(code.size_sym[g], 0, Part::High)));
        }
    }
    // the disk groups' records (their bytes defined once the link has placed the tables)
    for (std::size_t g = 0; g < n; g++) {
        if (!code.on_disk[g]) {
            continue;
        }
        out.push_back(MakeLabel(record[g]));
        for (std::size_t i = 0; i < 4; i++) {
            out.push_back(MakeByte(SymValue(code.rec_sym[4 * g + i])));
        }
    }
    EmitCopy(be, copy, code.copy);
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

std::uint64_t OverlayDiskCycles(std::uint32_t bytes) {
    return 115ull * bytes + 100000;
}

std::uint64_t OverlayLoadCycles(std::uint32_t bytes, std::size_t stubs) {
    // 16 a byte, a page's set-up, the enter routine and about 60 a stub put back and on
    return 16ull * bytes + 40ull * (bytes / 256 + 1) + 100 + 60ull * stubs;
}

} // namespace tisple
