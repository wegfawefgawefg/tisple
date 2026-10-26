#include "backend/gen.hpp"

namespace tisple {

namespace {

bool SameValue(const AsmValue& a, const AsmValue& b) {
    return a.sym == b.sym && a.offset == b.offset && a.part == b.part;
}

bool IsMemoryMode(Mode mode) {
    return mode != Mode::Imp && mode != Mode::Acc && mode != Mode::Imm && mode != Mode::Rel;
}

bool WritesMemory(Mn mn, Mode mode) {
    if (!IsMemoryMode(mode)) {
        return false;
    }
    switch (mn) {
    case Mn::STA:
    case Mn::STX:
    case Mn::STY:
    case Mn::INC:
    case Mn::DEC:
    case Mn::ASL:
    case Mn::LSR:
    case Mn::ROL:
    case Mn::ROR:
        return true;
    default:
        return false;
    }
}

// -1: the instruction does not write X or Y
int WrittenIndexReg(Mn mn) {
    switch (mn) {
    case Mn::LDX:
    case Mn::TAX:
    case Mn::INX:
    case Mn::DEX:
    case Mn::TSX:
        return 0;
    case Mn::LDY:
    case Mn::TAY:
    case Mn::INY:
    case Mn::DEY:
        return 1;
    default:
        return -1;
    }
}

void TrackIndexWrite(Gen& g, int r, Mn mn, Mode mode, const AsmValue& value) {
    RegContent& c = g.reg[r];
    if (mn == Mn::LDX || mn == Mn::LDY) {
        c.known = mode == Mode::Imm || ((mode == Mode::Abs || mode == Mode::Zp) && value.sym >= 0);
        c.mem = mode != Mode::Imm;
        c.v = value;
        c.delta = 0;
        return;
    }
    bool step = mn == Mn::INX || mn == Mn::DEX || mn == Mn::INY || mn == Mn::DEY;
    bool up = mn == Mn::INX || mn == Mn::INY;
    if (step && c.known && !c.mem && c.v.sym < 0) {
        c.v.offset = (c.v.offset + (up ? 1 : -1)) & 0xFF;
        return;
    }
    if (step && c.known && c.mem) {
        c.delta = static_cast<std::uint8_t>(c.delta + (up ? 1 : 0xFF));
        return;
    }
    c.known = false;
}

std::uint64_t LoopWeight(int depth) {
    if (depth > 6) {
        depth = 6;
    }
    return std::uint64_t(1) << (2 * depth);
}

// the register to index this byte with; mn decides which ones are possible
int PickIndexReg(const Gen& g, const Index& index, Mn mn) {
    if (index.is_reg) {
        return index.reg;
    }
    bool need_x = mn == Mn::LDY || mn == Mn::INC || mn == Mn::DEC || mn == Mn::ASL ||
                  mn == Mn::LSR || mn == Mn::ROL || mn == Mn::ROR;
    bool need_y = mn == Mn::LDX;
    for (int r = 0; r < 2; r++) {
        const RegContent& c = g.reg[r];
        bool allowed = r == 0 ? !need_y : !need_x;
        if (allowed && g.pin[r] < 0 && c.known && c.mem && SameValue(c.v, index.value)) {
            return r;
        }
    }
    if (need_x) {
        return 0;
    }
    if (need_y) {
        return 1;
    }
    if (g.pin[0] < 0) {
        return 0;
    }
    return 1;
}

// mode and address for byte k, with its index register loaded
Mode PrepareByte(Gen& g, const Opnd& o, int k, Mn mn, AsmValue& address) {
    const ByteRef& b = o.b[k];
    address = b.value;
    switch (b.kind) {
    case ByteKind::Imm:
        return Mode::Imm;
    case ByteKind::Mem:
        return Mode::Abs;
    case ByteKind::MemIdx: {
        int r = PickIndexReg(g, o.index, mn);
        LoadIndexReg(g, r, o.index);
        return r == 0 ? Mode::AbsX : Mode::AbsY;
    }
    case ByteKind::IndY:
        if (b.y_index) {
            LoadIndexReg(g, 1, o.index);
        } else {
            LoadYConst(g, b.y);
        }
        return Mode::IndY;
    case ByteKind::Reg:
        return Mode::Imp;
    }
    return Mode::Imp;
}

} // namespace

// only address-taken variables, globals and memory no variable declares
bool PointerMayReach(const Gen& g, const AsmValue& v) {
    if (v.sym < 0) {
        return true;
    }
    const SymInfo& info = g.be->info[static_cast<std::size_t>(v.sym)];
    if (info.role == SymRole::Var) {
        const Var& var = g.program->vars[static_cast<std::size_t>(info.owner)];
        return var.function < 0 || var.address_taken;
    }
    return info.role == SymRole::Data;
}

std::int32_t NewSym(Backend& be, const std::string& name, SymRole role, std::int32_t owner) {
    std::int32_t sym = NewSymbol(be.as, name);
    SymInfo info;
    info.role = role;
    info.owner = owner;
    be.info.push_back(info);
    return sym;
}

void Fail(Gen& g, const Loc& loc, const std::string& message) {
    if (!g.failed) {
        AddError(*g.be->errors, loc, message);
    }
    g.failed = true;
}

void Unsupported(Gen& g, ExprId id, const char* what) {
    Fail(g, ExprOf(*g.program, id).loc,
         std::string("the 6502 backend does not support ") + what + " yet");
}

void Emit(Gen& g, Mn mn, Mode mode, AsmValue value) {
    g.out->push_back(MakeIns(mn, mode, value));
    g.cost +=
        static_cast<std::uint64_t>(ItemCycles(g.be->as, g.out->back())) * LoopWeight(g.loop_depth);
    if (value.sym >= 0 && mode != Mode::Rel) {
        SymInfo& info = g.be->info[static_cast<std::size_t>(value.sym)];
        if (info.role != SymRole::Label) {
            info.weight += LoopWeight(g.loop_depth);
        }
    }
    if (mn == Mn::JSR) {
        if (g.pin[0] >= 0 || g.pin[1] >= 0) {
            g.violation = true;
        }
        ForgetRegs(g);
        return;
    }
    int r = WrittenIndexReg(mn);
    if (r >= 0) {
        if (g.pin[r] >= 0) {
            g.violation = true;
        }
        TrackIndexWrite(g, r, mn, mode, value);
    }
    if (!WritesMemory(mn, mode)) {
        return;
    }
    bool direct = mode == Mode::Abs || mode == Mode::Zp;
    bool pointer = mode == Mode::IndY || mode == Mode::IndX;
    for (RegContent& c : g.reg) {
        if (!c.known || !c.mem) {
            continue;
        }
        bool hit = direct ? SameValue(c.v, value) : !pointer || PointerMayReach(g, c.v);
        if (hit) {
            c.known = false;
        }
    }
}

namespace {

// INX/INY or DEX/DEY from one delta to another, if that takes only a few
bool StepIndexReg(Gen& g, int reg, std::uint8_t from, std::uint8_t to) {
    std::uint8_t up = static_cast<std::uint8_t>(to - from);
    if (!IsSmallDelta(up)) {
        return false;
    }
    bool forward = up < 0x80;
    int steps = forward ? up : 0x100 - up;
    for (int i = 0; i < steps; i++) {
        Mn mn = reg == 0 ? (forward ? Mn::INX : Mn::DEX) : (forward ? Mn::INY : Mn::DEY);
        Emit(g, mn, Mode::Imp);
    }
    return true;
}

// a constant into X or Y; one step from the constant it holds is INX/INY or DEX/DEY
void LoadIndexConst(Gen& g, int reg, std::uint8_t value) {
    const RegContent& c = g.reg[reg];
    if (g.pin[reg] < 0 && c.known && !c.mem && c.v.sym < 0) {
        std::uint8_t now = static_cast<std::uint8_t>(c.v.offset);
        std::uint8_t up = static_cast<std::uint8_t>(value - now);
        if (up == 0) {
            return;
        }
        if (up == 1 || up == 0xFF) {
            StepIndexReg(g, reg, now, value);
            return;
        }
    }
    Emit(g, reg == 0 ? Mn::LDX : Mn::LDY, Mode::Imm, Num(value));
}

} // namespace

bool CarryClearNow(const Gen& g) {
    if (g.out->empty()) {
        return false;
    }
    const AsmItem& last = g.out->back();
    if (last.kind != ItemKind::Ins || IsBranch(last.mn)) {
        return false;
    }
    return last.mn == Mn::CLC || last.carry == Carry::Clear;
}

bool AHoldsByte(const Gen& g, const Opnd& o, int k) {
    const ByteRef& b = o.b[k];
    if (o.in_a || b.kind != ByteKind::Mem || b.value.sym < 0) {
        return false;
    }
    for (std::size_t i = g.out->size(); i-- > 0;) {
        const AsmItem& item = (*g.out)[i];
        if (item.kind == ItemKind::Note) {
            continue;
        }
        bool access = item.kind == ItemKind::Ins && (item.mn == Mn::STA || item.mn == Mn::LDA) &&
                      (item.mode == Mode::Abs || item.mode == Mode::Zp);
        return access && SameValue(item.value, b.value);
    }
    return false;
}

std::uint32_t MaxOf(const Gen& g, ExprId id) {
    return ExprMax(*g.program, g.be->ranges, id);
}

std::int32_t NewLabel(Gen& g) {
    return NewSym(*g.be, "L" + std::to_string(g.be->as.symbols.size()), SymRole::Label, -1);
}

void PlaceLabel(Gen& g, std::int32_t label) {
    g.out->push_back(MakeLabel(label));
    ForgetRegs(g);
}

void Branch(Gen& g, Mn mn, std::int32_t label) {
    Emit(g, mn, Mode::Rel, SymValue(label));
}

void Jump(Gen& g, std::int32_t label) {
    Emit(g, Mn::JMP, Mode::Abs, SymValue(label));
}

void ForgetRegs(Gen& g) {
    g.reg[0].known = false;
    g.reg[1].known = false;
}

void SourceNote(Gen& g, const Loc& loc) {
    const Sources& sources = g.program->sources;
    if (loc.file >= sources.files.size()) {
        return;
    }
    const SourceFile& file = sources.files[loc.file];
    SourcePos pos = PosFromOffset(file, loc.span.start);
    if (loc.file == g.last_file && pos.line == g.last_line) {
        return;
    }
    g.last_file = loc.file;
    g.last_line = pos.line;
    std::uint32_t start = file.line_starts.empty() ? 0 : file.line_starts[pos.line - 1];
    std::size_t end = file.text.find('\n', start);
    std::string text = file.text.substr(start, end == std::string::npos ? end : end - start);
    std::size_t first = text.find_first_not_of(' ');
    text = first == std::string::npos ? "" : text.substr(first);
    AsmItem note;
    note.kind = ItemKind::Note;
    note.note = static_cast<std::int32_t>(g.be->as.notes.size());
    g.be->as.notes.push_back(file.path + ":" + std::to_string(pos.line) + ": " + text);
    g.out->push_back(note);
}

void LoadIndexReg(Gen& g, int reg, const Index& index) {
    if (index.is_reg) {
        if (index.reg != reg || index.delta != 0) {
            g.violation = true; // no way to move X to Y without A, nor to change a counter
        }
        return;
    }
    RegContent c = g.reg[reg];
    if (g.pin[reg] < 0 && c.known && c.mem && SameValue(c.v, index.value) &&
        StepIndexReg(g, reg, c.delta, index.delta)) {
        return;
    }
    Emit(g, reg == 0 ? Mn::LDX : Mn::LDY, Mode::Abs, index.value);
    if (!StepIndexReg(g, reg, 0, index.delta)) {
        Fail(g, Loc(), "internal: an index delta too big for INX/INY");
    }
}

void LoadYConst(Gen& g, std::uint8_t value) {
    LoadIndexConst(g, 1, value);
}

void LoadXConst(Gen& g, std::uint8_t value) {
    LoadIndexConst(g, 0, value);
}

void LoadA(Gen& g, const Opnd& o, int k) {
    if (o.in_a) {
        return;
    }
    const ByteRef& b = o.b[k];
    if (b.kind == ByteKind::Reg) {
        Emit(g, b.reg == 0 ? Mn::TXA : Mn::TYA, Mode::Imp);
        return;
    }
    AsmValue address;
    Mode mode = PrepareByte(g, o, k, Mn::LDA, address);
    Emit(g, Mn::LDA, mode, address);
}

void OpA(Gen& g, Mn mn, const Opnd& o, int k) {
    const ByteRef& b = o.b[k];
    if (b.kind == ByteKind::Reg) {
        AsmValue scratch = SymValue(g.be->scratch);
        Emit(g, b.reg == 0 ? Mn::STX : Mn::STY, Mode::Abs, scratch);
        Emit(g, mn, Mode::Abs, scratch);
        return;
    }
    AsmValue address;
    Mode mode = PrepareByte(g, o, k, mn, address);
    Emit(g, mn, mode, address);
}

void StoreA(Gen& g, const Opnd& dst, int k) {
    AsmValue address;
    Mode mode = PrepareByte(g, dst, k, Mn::STA, address);
    Emit(g, Mn::STA, mode, address);
}

bool CanRmw(const Gen& g, const Opnd& dst, int k) {
    const ByteRef& b = dst.b[k];
    if (b.kind == ByteKind::Mem) {
        return true;
    }
    if (b.kind != ByteKind::MemIdx) {
        return false;
    }
    if (dst.index.is_reg) {
        return dst.index.reg == 0;
    }
    return g.pin[0] < 0;
}

bool RmwByte(Gen& g, Mn mn, const Opnd& dst, int k) {
    if (!CanRmw(g, dst, k)) {
        return false;
    }
    AsmValue address;
    Mode mode = PrepareByte(g, dst, k, mn, address);
    Emit(g, mn, mode, address);
    return true;
}

TempMark MarkTemps(const Gen& g) {
    TempMark mark;
    mark.temp = g.temp_top;
    mark.ptemp = g.ptemp_top;
    return mark;
}

void ReleaseTemps(Gen& g, TempMark mark) {
    g.temp_top = mark.temp;
    g.ptemp_top = mark.ptemp;
}

Opnd NewTemp(Gen& g, std::uint32_t size) {
    std::uint32_t at = g.temp_top;
    g.temp_top += size;
    std::uint32_t& max = g.be->temp_size[static_cast<std::size_t>(g.function)];
    if (g.temp_top > max) {
        max = g.temp_top;
    }
    Opnd o = MemOpnd(SymValue(g.be->temp_sym[static_cast<std::size_t>(g.function)],
                              static_cast<std::int32_t>(at)),
                     size, 1);
    o.temp = true;
    return o;
}

AsmValue NewPtrTemp(Gen& g) {
    std::uint32_t at = g.ptemp_top;
    g.ptemp_top += 2;
    std::uint32_t& max = g.be->ptemp_size[static_cast<std::size_t>(g.function)];
    if (g.ptemp_top > max) {
        max = g.ptemp_top;
    }
    return SymValue(g.be->ptemp_sym[static_cast<std::size_t>(g.function)],
                    static_cast<std::int32_t>(at));
}

} // namespace tisple
