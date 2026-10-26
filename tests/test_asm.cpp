// the assembler: every encoding against the disassembler and the simulator's cycle counts

#include "asm/asm.hpp"
#include "emu/disasm.hpp"
#include "emu/harness.hpp"
#include "test_backend.hpp"

#include <cstdio>
#include <memory>

namespace tisple_tests {

namespace {

using tisple::Mn;
using tisple::Mode;

// what the chips disassembler prints for this mode and operand
std::string Expected(Mn mn, Mode mode, std::uint32_t v) {
    char text[32];
    const char* name = tisple::MnName(mn);
    switch (mode) {
    case Mode::Imp:
    case Mode::Acc:
        std::snprintf(text, sizeof(text), "%s", name);
        break;
    case Mode::Imm:
        std::snprintf(text, sizeof(text), "%s #$%02X", name, v);
        break;
    case Mode::Zp:
        std::snprintf(text, sizeof(text), "%s $%02X", name, v);
        break;
    case Mode::ZpX:
        std::snprintf(text, sizeof(text), "%s $%02X,X", name, v);
        break;
    case Mode::ZpY:
        std::snprintf(text, sizeof(text), "%s $%02X,Y", name, v);
        break;
    case Mode::Abs:
    case Mode::Rel:
        std::snprintf(text, sizeof(text), "%s $%04X", name, v);
        break;
    case Mode::AbsX:
        std::snprintf(text, sizeof(text), "%s $%04X,X", name, v);
        break;
    case Mode::AbsY:
        std::snprintf(text, sizeof(text), "%s $%04X,Y", name, v);
        break;
    case Mode::Ind:
        std::snprintf(text, sizeof(text), "%s ($%04X)", name, v);
        break;
    case Mode::IndX:
        std::snprintf(text, sizeof(text), "%s ($%02X,X)", name, v);
        break;
    case Mode::IndY:
        std::snprintf(text, sizeof(text), "%s ($%02X),Y", name, v);
        break;
    default:
        text[0] = 0;
        break;
    }
    return text;
}

// the flags that make a branch fall through
std::uint8_t NotTaken(Mn mn) {
    switch (mn) {
    case Mn::BPL:
        return tisple::kFlagN;
    case Mn::BVC:
        return tisple::kFlagV;
    case Mn::BCC:
        return tisple::kFlagC;
    case Mn::BNE:
        return tisple::kFlagZ;
    default:
        return 0;
    }
}

void TestEveryOpcode(TestResults& results) {
    int documented = 0;
    int mismatches = 0;
    auto m = std::make_unique<tisple::Machine>();
    for (int op = 0; op < 256; op++) {
        const tisple::OpcodeInfo& info = tisple::OpcodeInfoFor(static_cast<std::uint8_t>(op));
        bool doc = tisple::IsDocumentedOpcode(static_cast<std::uint8_t>(op));
        if (doc != info.valid) {
            std::fprintf(stderr, "opcode $%02X: documented %d, in the table %d\n", op, doc,
                         info.valid);
            mismatches++;
            continue;
        }
        if (!info.valid) {
            continue;
        }
        documented++;
        // assemble it at $0800 and read it back
        tisple::Assembly as;
        std::uint32_t operand = tisple::ModeSize(info.mode) == 3 ? 0x1234 : 0x42;
        if (info.mode == Mode::Rel) {
            operand = 0x0812;
        }
        as.items.push_back(
            tisple::MakeIns(info.mn, info.mode, tisple::Num(static_cast<std::int32_t>(operand))));
        if (info.mode == Mode::Abs && operand < 0x100) {
            continue;
        }
        tisple::LayoutCode(as, 0x0800);
        std::vector<std::uint8_t> bytes;
        std::string error;
        bool encoded = tisple::EncodeCode(as, bytes, error);
        int length = 0;
        tisple::InitMachine(*m);
        tisple::LoadBytes(*m, 0x0800, bytes.data(), bytes.size());
        std::string text = tisple::Disassemble(*m, 0x0800, length);
        std::string want = Expected(info.mn, info.mode, operand);
        if (!encoded || bytes.empty() || bytes[0] != op ||
            length != static_cast<int>(bytes.size()) || text != want) {
            std::fprintf(stderr, "opcode $%02X: assembled \"%s\" (%d bytes), want \"%s\"\n", op,
                         text.c_str(), length, want.c_str());
            mismatches++;
            continue;
        }
        // cycles: run it once with nothing crossing a page and branches falling through
        tisple::Regs regs;
        regs.pc = 0x0800;
        regs.p = static_cast<std::uint8_t>(regs.p | NotTaken(info.mn));
        regs.s = 0xF0;
        regs.x = 0;
        regs.y = 0;
        tisple::SetRegs(*m, regs);
        std::uint64_t before = m->cycles;
        tisple::StepInstruction(*m);
        int cycles = static_cast<int>(m->cycles - before);
        if (cycles != info.cycles) {
            std::fprintf(stderr, "opcode $%02X %s: %d cycles in the table, %d in the simulator\n",
                         op, want.c_str(), info.cycles, cycles);
            mismatches++;
        }
    }
    CHECK(results, documented == 151);
    CHECK(results, mismatches == 0);
}

void TestZeroPageShrink(TestResults& results) {
    tisple::Assembly as;
    std::int32_t zp = tisple::NewSymbol(as, "zp");
    std::int32_t far = tisple::NewSymbol(as, "far");
    as.symbols[static_cast<std::size_t>(zp)].zp = true;
    tisple::DefineSymbol(as, zp, 0x80);
    tisple::DefineSymbol(as, far, 0x1234);
    as.items.push_back(tisple::MakeIns(Mn::LDA, Mode::Abs, tisple::SymValue(zp, 1)));
    as.items.push_back(tisple::MakeIns(Mn::LDA, Mode::AbsX, tisple::SymValue(zp)));
    as.items.push_back(tisple::MakeIns(Mn::LDA, Mode::Abs, tisple::SymValue(far)));
    as.items.push_back(
        tisple::MakeIns(Mn::LDA, Mode::Imm, tisple::SymValue(far, 0, tisple::Part::High)));
    as.items.push_back(tisple::MakeIns(Mn::STA, Mode::IndY, tisple::SymValue(zp)));
    std::uint32_t end = tisple::LayoutCode(as, 0x6000);
    std::vector<std::uint8_t> bytes;
    std::string error;
    CHECK(results, tisple::EncodeCode(as, bytes, error));
    std::vector<std::uint8_t> want = {0xA5, 0x81, 0xBD, 0x80, 0x00, 0xAD,
                                      0x34, 0x12, 0xA9, 0x12, 0x91, 0x80};
    CHECK(results, bytes == want);
    CHECK(results, end == 0x6000 + want.size());
}

void TestBranchRelaxation(TestResults& results) {
    tisple::Assembly as;
    std::int32_t near = tisple::NewSymbol(as, "near");
    std::int32_t far = tisple::NewSymbol(as, "far");
    as.items.push_back(tisple::MakeLabel(near));
    as.items.push_back(tisple::MakeIns(Mn::BNE, Mode::Rel, tisple::SymValue(near)));
    as.items.push_back(tisple::MakeIns(Mn::BEQ, Mode::Rel, tisple::SymValue(far)));
    for (int i = 0; i < 200; i++) {
        as.items.push_back(tisple::MakeIns(Mn::NOP, Mode::Imp));
    }
    as.items.push_back(tisple::MakeLabel(far));
    as.items.push_back(tisple::MakeIns(Mn::BCC, Mode::Rel, tisple::SymValue(near)));
    tisple::LayoutCode(as, 0x6000);
    std::vector<std::uint8_t> bytes;
    std::string error;
    CHECK(results, tisple::EncodeCode(as, bytes, error));
    // BNE near (short), BEQ far as BNE +3 / JMP far, NOPs, BCC near as BCS +3 / JMP near
    CHECK(results, bytes.size() == 2 + 5 + 200 + 5);
    CHECK(results, bytes[0] == 0xD0 && bytes[1] == 0xFE);
    CHECK(results, bytes[2] == 0xD0 && bytes[3] == 0x03 && bytes[4] == 0x4C);
    std::uint16_t far_at = static_cast<std::uint16_t>(bytes[5] | (bytes[6] << 8));
    CHECK(results, far_at == 0x6000 + 207);
    CHECK(results, bytes[207] == 0xB0 && bytes[208] == 0x03 && bytes[209] == 0x4C);
    CHECK(results, bytes[210] == 0x00 && bytes[211] == 0x60);
    // and they really go there
    auto m = std::make_unique<tisple::Machine>();
    tisple::InitMachine(*m);
    tisple::LoadBytes(*m, 0x6000, bytes.data(), bytes.size());
    tisple::Regs regs;
    regs.pc = 0x6002;
    regs.p = static_cast<std::uint8_t>(regs.p | tisple::kFlagZ);
    tisple::SetRegs(*m, regs);
    tisple::StepInstruction(*m);
    tisple::StepInstruction(*m);
    CHECK(results, tisple::GetRegs(*m).pc == far_at);
}

void TestUndefinedSymbol(TestResults& results) {
    tisple::Assembly as;
    std::int32_t missing = tisple::NewSymbol(as, "missing");
    as.items.push_back(tisple::MakeIns(Mn::JMP, Mode::Abs, tisple::SymValue(missing)));
    tisple::LayoutCode(as, 0x6000);
    std::vector<std::uint8_t> bytes;
    std::string error;
    CHECK(results, !tisple::EncodeCode(as, bytes, error));
    CHECK(results, error == "undefined symbol missing");
}

} // namespace

void RunAsmTests(TestResults& results) {
    TestEveryOpcode(results);
    TestZeroPageShrink(results);
    TestBranchRelaxation(results);
    TestUndefinedSymbol(results);
}

} // namespace tisple_tests
