#include "backend/irq.hpp"

#include <algorithm>
#include <utility>

namespace tisple {

namespace {

// the runtime's and single-sequence scratch symbols, 2 bytes each (frames.cpp places them)
std::vector<std::int32_t> ScratchSyms(const Backend& be) {
    return {be.scratch, be.rt_a, be.rt_b, be.rt_r, be.rt_s};
}

bool IsScratch(const Backend& be, std::int32_t sym) {
    std::vector<std::int32_t> syms = ScratchSyms(be);
    return std::find(syms.begin(), syms.end(), sym) != syms.end();
}

bool IsRoutine(const Backend& be, std::int32_t sym) {
    for (int r = 0; r < kRuntimeCount; r++) {
        if (be.rt_sym[r] == sym) {
            return true;
        }
    }
    for (const DivRoutine& d : be.div_routines) {
        if (d.sym == sym) {
            return true;
        }
    }
    return false;
}

using Byte = std::pair<std::int32_t, std::int32_t>; // a scratch symbol and an offset in it

void AddByte(std::vector<Byte>& bytes, std::int32_t sym, std::int32_t offset) {
    Byte b{sym, offset & 1};
    if (std::find(bytes.begin(), bytes.end(), b) == bytes.end()) {
        bytes.push_back(b);
    }
}

// the scratch bytes one item may read or write; all of them for an indexed access (its byte
// depends on a register)
void ItemBytes(const Backend& be, const AsmItem& item, std::vector<Byte>& bytes) {
    if (item.kind == ItemKind::Wait && item.scratch.sym >= 0) {
        AddByte(bytes, item.scratch.sym, item.scratch.offset);
        AddByte(bytes, item.scratch.sym, item.scratch.offset + 1);
    }
    if (item.kind != ItemKind::Ins || !IsScratch(be, item.value.sym)) {
        return;
    }
    bool indexed = item.mode == Mode::ZpX || item.mode == Mode::ZpY || item.mode == Mode::AbsX ||
                   item.mode == Mode::AbsY;
    if (indexed) {
        for (std::int32_t sym : ScratchSyms(be)) {
            AddByte(bytes, sym, 0);
            AddByte(bytes, sym, 1);
        }
        return;
    }
    AddByte(bytes, item.value.sym, item.value.offset);
    // a pointer: both its bytes
    if (item.mode == Mode::IndX || item.mode == Mode::IndY || item.mode == Mode::Ind) {
        AddByte(bytes, item.value.sym, item.value.offset + 1);
    }
}

} // namespace

std::vector<bool> HandlerFunctions(const Program& program) {
    std::vector<bool> reached(program.functions.size(), false);
    if (program.handler < 0) {
        return reached;
    }
    std::vector<std::int32_t> work = {program.handler};
    reached[static_cast<std::size_t>(program.handler)] = true;
    while (!work.empty()) {
        std::int32_t f = work.back();
        work.pop_back();
        for (std::int32_t callee : program.functions[static_cast<std::size_t>(f)].callees) {
            if (!reached[static_cast<std::size_t>(callee)]) {
                reached[static_cast<std::size_t>(callee)] = true;
                work.push_back(callee);
            }
        }
    }
    return reached;
}

void EmitIrqEntry(Backend& be, const std::vector<bool>& kept, const std::vector<AsmItem>& runtime,
                  IrqPlan& plan, std::vector<AsmItem>& out) {
    const Program& program = *be.program;
    std::vector<bool> handler = HandlerFunctions(program);
    std::vector<Byte> bytes;
    bool routines = false;
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (!handler[f] || !kept[f]) {
            continue;
        }
        for (const AsmItem& item : be.code[f]) {
            ItemBytes(be, item, bytes);
            bool call = item.kind == ItemKind::Ins && (item.mn == Mn::JSR || item.mn == Mn::JMP);
            routines = routines || (call && IsRoutine(be, item.value.sym));
        }
    }
    if (routines) {
        for (const AsmItem& item : runtime) {
            ItemBytes(be, item, bytes);
        }
    }
    std::sort(bytes.begin(), bytes.end());

    plan.direct_sym = NewSym(be, "irq.direct", SymRole::Label, -1);
    plan.entry_sym = NewSym(be, "irq.entry", SymRole::Label, -1);
    plan.saved_sym = NewSym(be, "irq.saved", SymRole::Data, -1);
    out.push_back(MakeLabel(plan.direct_sym));
    out.push_back(MakeIns(Mn::STA, Mode::Zp, Num(kIrqSaveA)));
    out.push_back(MakeLabel(plan.entry_sym));
    out.push_back(MakeIns(Mn::CLD, Mode::Imp));
    out.push_back(MakeIns(Mn::TXA, Mode::Imp));
    out.push_back(MakeIns(Mn::PHA, Mode::Imp));
    out.push_back(MakeIns(Mn::TYA, Mode::Imp));
    out.push_back(MakeIns(Mn::PHA, Mode::Imp));
    for (const Byte& b : bytes) {
        out.push_back(MakeIns(Mn::LDA, Mode::Abs, SymValue(b.first, b.second)));
        out.push_back(MakeIns(Mn::PHA, Mode::Imp));
    }
    out.push_back(MakeIns(Mn::JSR, Mode::Abs,
                          SymValue(be.fn_sym[static_cast<std::size_t>(program.handler)])));
    for (std::size_t i = bytes.size(); i-- > 0;) {
        out.push_back(MakeIns(Mn::PLA, Mode::Imp));
        out.push_back(MakeIns(Mn::STA, Mode::Abs, SymValue(bytes[i].first, bytes[i].second)));
    }
    out.push_back(MakeIns(Mn::PLA, Mode::Imp));
    out.push_back(MakeIns(Mn::TAY, Mode::Imp));
    out.push_back(MakeIns(Mn::PLA, Mode::Imp));
    out.push_back(MakeIns(Mn::TAX, Mode::Imp));
    out.push_back(MakeIns(Mn::LDA, Mode::Zp, Num(kIrqSaveA)));
    out.push_back(MakeIns(Mn::RTI, Mode::Imp));
    out.push_back(MakeLabel(plan.saved_sym));
    out.push_back(MakeByte(Num(0)));
    out.push_back(MakeByte(Num(0)));
}

} // namespace tisple
