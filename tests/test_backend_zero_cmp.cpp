// a CMP, CPX or CPY #0 right after an instruction whose N and Z already show that register
// goes when nothing reads the carry it sets; a call or tail jump into our own code (a symbol)
// reads no flags, a jsr or jmp to an address may; a jump to a label reads what is read there
// (dead_regs.cpp)

#include "backend/peephole.hpp"
#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

using tisple::Mn;
using tisple::Mode;

struct Items {
    tisple::Assembly as;
    std::vector<tisple::AsmItem> items;
    std::int32_t var = -1;
    std::int32_t label = -1;
    std::int32_t routine = -1;
};

Items Start() {
    Items t;
    t.var = tisple::NewSymbol(t.as, "v");
    tisple::DefineSymbol(t.as, t.var, 0x80);
    t.label = tisple::NewSymbol(t.as, "l");
    t.routine = tisple::NewSymbol(t.as, "r");
    return t;
}

void Ins(Items& t, Mn mn, Mode mode, tisple::AsmValue v = tisple::AsmValue()) {
    t.items.push_back(tisple::MakeIns(mn, mode, v));
}

int Compares(const std::vector<tisple::AsmItem>& items) {
    int n = 0;
    for (const tisple::AsmItem& item : items) {
        bool cmp = item.mn == Mn::CMP || item.mn == Mn::CPX || item.mn == Mn::CPY;
        n += item.kind == tisple::ItemKind::Ins && cmp ? 1 : 0;
    }
    return n;
}

// LOAD v / CMP #0 / branch to l / then `after`, l: CLC / RTS, with l before the compare
// instead when `labelled`: how many compares are left
int Run(Mn load, Mn cmp, Mn branch, bool labelled, Mn after, bool own_call) {
    Items t = Start();
    Ins(t, load, load == Mn::INX || load == Mn::DEY || load == Mn::TAX ? Mode::Imp : Mode::Abs,
        tisple::SymValue(t.var));
    if (load == Mn::INX || load == Mn::DEY || load == Mn::TAX) {
        t.items.back().value = tisple::AsmValue();
    }
    if (labelled) {
        t.items.push_back(tisple::MakeLabel(t.label));
    }
    Ins(t, cmp, Mode::Imm, tisple::Num(0));
    Ins(t, branch, Mode::Rel, tisple::SymValue(t.label));
    if (after == Mn::JSR) {
        Ins(t, Mn::JSR, Mode::Abs, own_call ? tisple::SymValue(t.routine) : tisple::Num(0xFD0C));
        Ins(t, Mn::BCC, Mode::Rel, tisple::SymValue(t.label));
    } else if (after == Mn::BRK) {
        // a tail jump into a routine (or, not our own, to an address)
        Ins(t, Mn::JMP, Mode::Abs, own_call ? tisple::SymValue(t.routine) : tisple::Num(0xFD0C));
    } else if (after == Mn::JMP) {
        // a jump to l reads only what l does
        Ins(t, Mn::JMP, Mode::Abs, tisple::SymValue(t.label));
        Ins(t, Mn::ADC, Mode::Imm, tisple::Num(1));
    } else if (after != Mn::NOP) {
        Ins(t, after, Mode::Imm, tisple::Num(1));
        Ins(t, Mn::STA, Mode::Abs, tisple::SymValue(t.var));
    }
    if (!labelled) {
        t.items.push_back(tisple::MakeLabel(t.label));
    }
    // the return reads every flag: the carry is set first
    Ins(t, Mn::CLC, Mode::Imp);
    Ins(t, Mn::RTS, Mode::Imp);
    tisple::DropDeadValues(t.as, t.items);
    return Compares(t.items);
}

void TestPeephole(TestResults& results) {
    // N and Z already show the register; the carry is set again (CLC) before anything reads
    // it, or never read
    CHECK(results, Run(Mn::LDA, Mn::CMP, Mn::BEQ, false, Mn::NOP, true) == 0);
    CHECK(results, Run(Mn::AND, Mn::CMP, Mn::BNE, false, Mn::LDA, true) == 0);
    CHECK(results, Run(Mn::LDX, Mn::CPX, Mn::BEQ, false, Mn::NOP, true) == 0);
    CHECK(results, Run(Mn::INX, Mn::CPX, Mn::BNE, false, Mn::NOP, true) == 0);
    CHECK(results, Run(Mn::DEY, Mn::CPY, Mn::BMI, false, Mn::NOP, true) == 0);
    CHECK(results, Run(Mn::TAX, Mn::CPX, Mn::BEQ, false, Mn::NOP, true) == 0);
    CHECK(results, Run(Mn::LDA, Mn::CMP, Mn::BEQ, false, Mn::JMP, true) == 0);
    // ADC reads the carry the compare set
    CHECK(results, Run(Mn::LDA, Mn::CMP, Mn::BEQ, false, Mn::ADC, true) == 1);
    // the flags show another register, or a label lets other ways in
    CHECK(results, Run(Mn::LDX, Mn::CMP, Mn::BEQ, false, Mn::NOP, true) == 1);
    CHECK(results, Run(Mn::LDA, Mn::CPX, Mn::BEQ, false, Mn::NOP, true) == 1);
    CHECK(results, Run(Mn::TAX, Mn::CMP, Mn::BEQ, false, Mn::NOP, true) == 1);
    CHECK(results, Run(Mn::LDA, Mn::CMP, Mn::BEQ, true, Mn::NOP, true) == 1);
    // a call of a routine sets the carry before the BCC after it reads it; a jsr to an
    // address (a ROM routine) may read the carry it is called with
    CHECK(results, Run(Mn::LDA, Mn::CMP, Mn::BEQ, false, Mn::JSR, true) == 0);
    CHECK(results, Run(Mn::LDA, Mn::CMP, Mn::BEQ, false, Mn::JSR, false) == 1);
    // a tail jump into a routine reads no flags; one to an address may
    CHECK(results, Run(Mn::LDA, Mn::CMP, Mn::BEQ, false, Mn::BRK, true) == 0);
    CHECK(results, Run(Mn::LDA, Mn::CMP, Mn::BEQ, false, Mn::BRK, false) == 1);
}

void CheckBytes(TestResults& results, const std::string& text) {
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c);
    for (std::uint16_t x = 0; x < 256; x++) {
        ok = ok && DiffCall(d, "t.f", {x});
    }
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

void TestPrograms(TestResults& results) {
    // a match on a masked byte: the AND's flags test the 0 clause
    std::string text = "(let g u8 0)\n"
                       "(fn f ((x u8)) u8\n"
                       "    (match (& x 3) (0 (set g (+ g 1))) (2 (set g (* g x))) (_ (set g 7)))\n"
                       "    (match (& x 12) (0 (set g (+ g x))) (4 (set g (- g 3))) (_))\n"
                       "    g)";
    CheckBytes(results, text);
    Compiled c = CompileText(text);
    int zero = 0;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        for (std::size_t i = span.first; span.name == "t.f" && i < span.end; i++) {
            const tisple::AsmItem& item = c.code.as.items[i];
            zero += item.kind == tisple::ItemKind::Ins && item.mn == Mn::CMP &&
                    item.mode == Mode::Imm && item.value.sym < 0 && item.value.offset == 0;
        }
    }
    CHECK(results, zero == 0);
    // compares whose carry later code reads
    CheckBytes(results, "(let g u8 0)\n"
                        "(fn f ((x u8)) u8\n"
                        "    (if (= (& x 7) 0) (set g (+ g x)) (set g (- g x)))\n"
                        "    (+ g (if (= (>> x 1) 0) 1 x)))");
}

} // namespace

void RunBackendZeroCmpTests(TestResults& results) {
    TestPeephole(results);
    TestPrograms(results);
}

} // namespace tisple_tests
