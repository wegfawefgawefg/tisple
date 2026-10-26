// the gap finder's profiler: known-redundant instructions in hand-made code are flagged,
// necessary ones are not, and programs/life's inner-loop CLCs show up

#include "test_gaps.hpp"

#include "emu/harness.hpp"
#include "fuzz_gen.hpp"
#include "gaps/gaps.hpp"
#include "test_backend.hpp"

#include <memory>

namespace tisple_tests {

namespace {

using tisple::Mn;
using tisple::Mode;

// hand-made code as a compiled program: one span, assembled at $0800
struct HandMade {
    tisple::Compiled compiled;
    std::int32_t loop = -1;

    void Ins(Mn mn, Mode mode, std::int32_t operand = 0) {
        compiled.as.items.push_back(tisple::MakeIns(mn, mode, tisple::Num(operand)));
    }
};

// 32 times round:
//   SEC / LDA #$F0 / CLC (needed: C=1) / ADC #$20 (carries) / STA $10
//   LDA #$01 (needed) / CLC (needed: C=1) / ADC #$01 / CLC (redundant: C=0) / ADC #$02
//   STA $11 / LDA $11 (redundant) / STA $12 (dead) / LDA #$07 / STA $12 / LDY $12
//   LDA #$09 (nothing reads A or its flags) / DEX / BNE
void BuildLoop(HandMade& h) {
    tisple::Assembly& as = h.compiled.as;
    h.loop = tisple::NewSymbol(as, "loop");
    h.Ins(Mn::LDX, Mode::Imm, 0x20);
    as.items.push_back(tisple::MakeLabel(h.loop));
    h.Ins(Mn::SEC, Mode::Imp);
    h.Ins(Mn::LDA, Mode::Imm, 0xF0);
    h.Ins(Mn::CLC, Mode::Imp);
    h.Ins(Mn::ADC, Mode::Imm, 0x20);
    h.Ins(Mn::STA, Mode::Zp, 0x10);
    h.Ins(Mn::LDA, Mode::Imm, 0x01);
    h.Ins(Mn::CLC, Mode::Imp);
    h.Ins(Mn::ADC, Mode::Imm, 0x01);
    h.Ins(Mn::CLC, Mode::Imp);
    h.Ins(Mn::ADC, Mode::Imm, 0x02);
    h.Ins(Mn::STA, Mode::Zp, 0x11);
    h.Ins(Mn::LDA, Mode::Zp, 0x11);
    h.Ins(Mn::STA, Mode::Zp, 0x12);
    h.Ins(Mn::LDA, Mode::Imm, 0x07);
    h.Ins(Mn::STA, Mode::Zp, 0x12);
    h.Ins(Mn::LDY, Mode::Zp, 0x12);
    h.Ins(Mn::LDA, Mode::Imm, 0x09);
    h.Ins(Mn::DEX, Mode::Imp);
    as.items.push_back(tisple::MakeIns(Mn::BNE, Mode::Rel, tisple::SymValue(h.loop)));
    h.Ins(Mn::BRK, Mode::Imp);
    tisple::LayoutCode(as, 0x0800);
    std::string error;
    tisple::EncodeCode(as, h.compiled.image, error);
    h.compiled.origin = 0x0800;
    tisple::FunctionSpan span;
    span.name = "t";
    span.first = 0;
    span.end = as.items.size();
    h.compiled.spans.push_back(span);
}

std::uint16_t AddressOf(const HandMade& h, std::size_t item) {
    return h.compiled.as.items[item].address;
}

bool Has(const std::vector<tisple::Finding>& findings, tisple::Pattern pattern,
         std::uint16_t address) {
    for (const tisple::Finding& f : findings) {
        if (f.pattern == pattern && f.address == address) {
            return true;
        }
    }
    return false;
}

bool AnyAt(const std::vector<tisple::Finding>& findings, std::uint16_t address) {
    for (const tisple::Finding& f : findings) {
        if (f.address == address) {
            return true;
        }
    }
    return false;
}

void TestHandMadeLoop(TestResults& results) {
    HandMade h;
    BuildLoop(h);
    auto m = std::make_unique<tisple::Machine>();
    tisple::InitMachine(*m);
    tisple::LoadBytes(*m, 0x0800, h.compiled.image.data(), h.compiled.image.size());
    tisple::Regs regs;
    regs.pc = 0x0800;
    tisple::SetRegs(*m, regs);
    auto p = std::make_unique<tisple::Profiler>();
    tisple::SetBusLog(*m, &p->bus);
    tisple::RunOptions options;
    options.trace = tisple::ProfileTrace;
    options.trace_ctx = p.get();
    tisple::RunResult r = tisple::Run(*m, options);
    tisple::FinishProfile(*p, *m);
    CHECK(results, r.reason == tisple::StopReason::kBrk);

    auto map = std::make_unique<tisple::SiteMap>();
    tisple::BuildSiteMap(h.compiled, *map);
    std::vector<tisple::Finding> found =
        tisple::FindGaps(h.compiled, *map, *p, tisple::FindOptions());
    // items: 0 LDX, 1 label, 2 SEC, 3 LDA, 4 CLC, 5 ADC, 6 STA $10, 7 LDA #1, 8 CLC, 9 ADC,
    // 10 CLC, 11 ADC, 12 STA $11, 13 LDA $11, 14 STA $12, 15 LDA #7, 16 STA $12, 17 LDY,
    // 18 LDA #9, 19 DEX, 20 BNE, 21 BRK
    CHECK(results, p->sites[AddressOf(h, 10)].count == 32);
    CHECK(results, Has(found, tisple::Pattern::kFlagAlready, AddressOf(h, 10)));
    CHECK(results, Has(found, tisple::Pattern::kLoadAlready, AddressOf(h, 13)));
    CHECK(results, Has(found, tisple::Pattern::kStoreDead, AddressOf(h, 14)));
    CHECK(results, Has(found, tisple::Pattern::kDeadValue, AddressOf(h, 18)));
    // the loaded values that are used are not dead
    CHECK(results, !Has(found, tisple::Pattern::kDeadValue, AddressOf(h, 3)));
    CHECK(results, !Has(found, tisple::Pattern::kDeadValue, AddressOf(h, 15)));
    // necessary: the carry was set, the register held something else, the byte is read
    CHECK(results, !AnyAt(found, AddressOf(h, 4)));
    CHECK(results, !AnyAt(found, AddressOf(h, 8)));
    CHECK(results, !AnyAt(found, AddressOf(h, 7)));
    CHECK(results, !Has(found, tisple::Pattern::kStoreDead, AddressOf(h, 16)));
    // the loop branch went both ways; LDY $12 held 7 already on all but the first time (and
    // nothing reads Y: a dead value)
    CHECK(results, !AnyAt(found, AddressOf(h, 20)));
    CHECK(results, !Has(found, tisple::Pattern::kLoadAlready, AddressOf(h, 17)));
    CHECK(results, Has(found, tisple::Pattern::kDeadValue, AddressOf(h, 17)));
    // a lower bar: 31 of 32 is enough
    tisple::FindOptions loose;
    loose.min_fraction = 0.9;
    found = tisple::FindGaps(h.compiled, *map, *p, loose);
    CHECK(results, Has(found, tisple::Pattern::kLoadAlready, AddressOf(h, 17)));
}

void TestLifeCarries(TestResults& results) {
    Compiled c = CompileFile("programs/life/main.tsp");
    CHECK(results, c.ok);
    if (!c.ok) {
        return;
    }
    tisple::GapsOptions options;
    options.run.max_cycles = 3000000;
    tisple::WindowCache cache;
    tisple::ProgramGaps gaps;
    std::string error;
    CHECK(results, tisple::FindProgramGaps(c.built.program, c.code, options, cache, gaps, error));
    int step_carries = 0;
    for (const tisple::Finding& f : gaps.findings) {
        if (f.pattern == tisple::Pattern::kFlagAlready && f.function == "main.step" &&
            f.text == "CLC" && f.source.find("(let n u8") != std::string::npos) {
            step_carries++;
        }
    }
    // the eight neighbour counts are 0 or 1: none of the seven additions carries
    CHECK(results, step_carries == 7);
    // and the row pointers step by 42
    bool strided = false;
    for (const tisple::Finding& f : gaps.findings) {
        strided = strided || (f.pattern == tisple::Pattern::kStrided && f.function == "main.step" &&
                              f.reason.find("+42") != std::string::npos);
    }
    CHECK(results, strided);
}

void TestFuzzDeterministic(TestResults& results) {
    tisple::GapsOptions options;
    options.run.max_cycles = 200000;
    tisple::BulkGaps one = tisple::FuzzGaps(GenerateProgram, 1, 6, 1, options);
    tisple::BulkGaps two = tisple::FuzzGaps(GenerateProgram, 1, 6, 2, options);
    CHECK(results, one.programs == 6 && one.profiled + one.failed == 6);
    CHECK(results, one.saved == two.saved && one.cycles == two.cycles);
    CHECK(results, tisple::FormatBulkReport(one, 1, 10) == tisple::FormatBulkReport(two, 1, 10));
}

} // namespace

void RunGapsTests(TestResults& results) {
    TestHandMadeLoop(results);
    TestLifeCarries(results);
    TestFuzzDeterministic(results);
}

} // namespace tisple_tests
