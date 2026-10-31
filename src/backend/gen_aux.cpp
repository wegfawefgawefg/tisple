// with_aux's switches (check/aux_mode.hpp). RAMRD is $C002 off, $C003 on; RAMWRT $C004 off,
// $C005 on; a store of anything sets one. On: a constant bool is one STA or none, a variable
// one indexes the pair (LDX b / STA $C002,X, as a bool is 0 or 1), through Y when X holds a
// loop counter, else a branch over the STA. Off: the value worked out first, into A or a
// temporary (never into the caller's destination, which may be a global the switches would
// send to aux memory), then STA $C002 / STA $C004 for the switches the region may turn on.
// The stores are to numbers, which no pass tracks or drops; the registers are forgotten after
// them, and in the functions that run with them on the peephole tracks only zero page
// (peephole.hpp), so nothing known of other memory crosses a switch.

#include "backend/gen.hpp"
#include "check/aux_mode.hpp"

namespace tisple {

namespace {

constexpr std::uint16_t kRamrdOff = 0xC002;
constexpr std::uint16_t kRamwrtOff = 0xC004;

// one switch on when flag is true: base is its off address, base + 1 its on one
void SwitchOn(Gen& g, ExprId flag, std::uint16_t base) {
    std::uint16_t value = 0;
    if (ConstOf(g, flag, value)) {
        if (value != 0) {
            Emit(g, Mn::STA, Mode::Abs, Num(base + 1));
        }
        return;
    }
    int reg = g.pin[0] < 0 ? 0 : (g.pin[1] < 0 ? 1 : -1);
    Opnd o = Eval(g, flag, nullptr);
    bool memory = !o.in_a && o.size == 1 && o.b[0].kind == ByteKind::Mem;
    if (reg >= 0 && memory) {
        Index index;
        index.used = true;
        index.value = o.b[0].value;
        LoadIndexReg(g, reg, index);
    } else {
        LoadA(g, o, 0);
        if (reg < 0) {
            // both index registers hold loop counters: a branch over the store
            std::int32_t skip = NewLabel(g);
            Emit(g, Mn::CMP, Mode::Imm, Num(0));
            Branch(g, Mn::BEQ, skip);
            Emit(g, Mn::STA, Mode::Abs, Num(base + 1));
            PlaceLocalLabel(g, skip);
            return;
        }
        Emit(g, reg == 0 ? Mn::TAX : Mn::TAY, Mode::Imp);
        g.reg[reg].known = false;
    }
    Emit(g, Mn::STA, reg == 0 ? Mode::AbsX : Mode::AbsY, Num(base));
}

void SwitchesOff(Gen& g, std::uint32_t bits) {
    if ((bits & kAuxReads) != 0) {
        Emit(g, Mn::STA, Mode::Abs, Num(kRamrdOff));
    }
    if ((bits & kAuxWrites) != 0) {
        Emit(g, Mn::STA, Mode::Abs, Num(kRamwrtOff));
    }
}

// the switches' stores leave the registers holding what they held, but what they knew of
// memory outside zero page is another memory's now
void AfterSwitch(Gen& g) {
    for (int r = 0; r < 2; r++) {
        if (g.pin[r] < 0) {
            g.reg[r].known = false;
        }
    }
}

} // namespace

void GenAuxSwitch(Gen& g, ExprId id) {
    const Expr& e = ExprOf(*g.program, id);
    if (e.value == 0) {
        EvalAuxOff(g, e, nullptr);
        return;
    }
    ExprId reads = e.a;
    ExprId writes = e.b;
    SwitchOn(g, reads, kRamrdOff);
    SwitchOn(g, writes, kRamwrtOff);
    AfterSwitch(g);
}

Opnd EvalAuxOff(Gen& g, const Expr& e, const Opnd* dst) {
    (void)dst; // the value goes where the switches cannot send it elsewhere
    Opnd value = ImmOpnd(0, 1);
    std::uint32_t bits = e.ref;
    bool has_value = e.a >= 0 && e.type != kVoidType && e.type != kNeverType;
    if (e.a >= 0 && !has_value) {
        GenStmt(g, e.a);
    }
    if (has_value) {
        std::uint32_t size = TypeOf(*g.program, e.type).size;
        Opnd o = Eval(g, e.a, nullptr);
        if (size == 1) {
            LoadA(g, o, 0);
            value = InA();
        } else {
            value = o.temp ? o : NewTemp(g, 2);
            if (!o.temp) {
                CopyOpnd(g, o, value);
            }
        }
    }
    SwitchesOff(g, bits);
    AfterSwitch(g);
    return value;
}

} // namespace tisple
