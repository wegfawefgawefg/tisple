// lib/machine on the built-in machine: each model the rom-less machine says it is, a 64 KB iie,
// Apple's roms when the owner has them, the cards in the slots, detect run from the language
// card (both banks), and the memory it borrows given back

#include "machine/disk2.hpp"
#include "machine/mockingboard.hpp"
#include "machine/mouse.hpp"
#include "test_apple2_libs.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

const char* const kDetect = "(import machine)\n"
                            "(let out (array u8 16) :address 0x0300)\n"
                            "(fn main () void\n"
                            "    (machine.detect)\n"
                            "    (set (at out 0) machine.model)\n"
                            "    (set (at out 1) machine.kb)\n"
                            "    (set (at out 2) (if machine.aux 1 0))\n"
                            "    (set (at out 3) (if machine.col80 1 0))\n"
                            "    (set (at out 4) (if machine.cmos 1 0))\n"
                            "    (set (at out 5) (if machine.mousetext 1 0))\n"
                            "    (set (at out 6) (machine.find machine.MOUSE))\n"
                            "    (for n u8 1 8\n"
                            "        (set (at out (+ 7 n)) (machine.card n))))\n";

// lib/machine's numbers
enum : int { kUnknown, kII, kIIPlus, kIIe, kIIeEnhanced, kIIc, kIIcPlus, kIIgs };
enum : int { kNone, kDisk2, kStorage, kMouse, kMockingboard, kSerial, kCol80, kOther };

struct Found {
    int model = -1;
    int kb = 0;
    bool aux = false;
    bool col80 = false;
    bool cmos = false;
    bool mousetext = false;
    int mouse_slot = 0;
    int card[8] = {};
};

Found Read(const tisple::Apple2& a) {
    Found f;
    const uint8_t* out = a.cpu.mem + 0x0300;
    f.model = out[0];
    f.kb = out[1];
    f.aux = out[2] != 0;
    f.col80 = out[3] != 0;
    f.cmos = out[4] != 0;
    f.mousetext = out[5] != 0;
    f.mouse_slot = out[6];
    for (int n = 1; n < 8; n++) {
        f.card[n] = out[7 + n];
    }
    return f;
}

struct Setup {
    tisple::MachineModel model = tisple::MachineModel::kIIeEnhanced;
    bool aux_ram = true;
    const char* rom = nullptr; // a file in the owner's roms; skipped when missing
    bool mouse = true;
    int mockingboard = 0;
    bool disk2 = false;
    tisple::Cpu cpu = tisple::Cpu::k6502;
};

bool Detect(TestResults& results, const LibBuild& b, const Setup& s, Found& f) {
    std::unique_ptr<tisple::Apple2> a = LoadLibProgram(b);
    a->cpu.cpu_kind = s.cpu;
    a->model = s.model;
    a->aux_ram = s.aux_ram;
    if (s.rom != nullptr) {
        std::vector<uint8_t> rom;
        std::string error;
        if (!ReadRomFile(s.rom, rom) || !tisple::LoadRom(*a, rom.data(), rom.size(), error)) {
            return false;
        }
    }
    if (s.mouse) {
        tisple::InsertMouseCard(*a, 4, nullptr);
    }
    if (s.mockingboard != 0) {
        tisple::InsertMockingboard(*a, s.mockingboard);
    }
    if (s.disk2) {
        tisple::InsertDisk2Card(*a, 6, nullptr);
    }
    // what detect must give back: aux memory's $0800, $0C00 and $0400, main's $0400
    a->aux[0x0800] = 0x12;
    a->aux[0x0C00] = 0x34;
    a->aux[0x0400] = 0x56;
    a->cpu.mem[0x0400] = 0xC1;
    if (!RunToReturn(results, *a, 2000000)) {
        return false;
    }
    CHECK(results, a->aux[0x0800] == 0x12 && a->aux[0x0C00] == 0x34 && a->aux[0x0400] == 0x56);
    CHECK(results, a->cpu.mem[0x0400] == 0xC1);
    CHECK(results, !a->sw.store80 && !a->sw.page2 && !a->sw.ramrd && !a->sw.ramwrt);
    f = Read(*a);
    return true;
}

void TestModels(TestResults& results, const LibBuild& b) {
    Found f;
    Setup s;
    if (Detect(results, b, s, f)) {
        CHECK(results, f.model == kIIeEnhanced && f.kb == 128 && f.aux && f.col80);
        CHECK(results, !f.cmos && f.mousetext); // the nmos 6502 the test machine starts with
        CHECK(results, f.card[3] == kCol80 && f.card[4] == kMouse && f.mouse_slot == 4);
        CHECK(results, f.card[1] == kNone && f.card[6] == kNone && f.card[7] == kNone);
    }
    s.model = tisple::MachineModel::kIIe;
    if (Detect(results, b, s, f)) {
        CHECK(results, f.model == kIIe && f.kb == 128 && f.aux && !f.mousetext);
    }
    s.model = tisple::MachineModel::kIIc;
    if (Detect(results, b, s, f)) {
        CHECK(results, f.model == kIIc && f.kb == 128 && f.aux && f.mousetext);
    }
    s.model = tisple::MachineModel::kIIgs;
    if (Detect(results, b, s, f)) {
        CHECK(results, f.model == kIIgs && f.kb == 128 && f.aux && f.mousetext);
    }
    s.model = tisple::MachineModel::kIIPlus;
    if (Detect(results, b, s, f)) {
        CHECK(results, f.model == kIIPlus && f.kb == 48 && !f.aux && !f.col80 && !f.mousetext);
        CHECK(results, f.card[3] == kNone && f.card[4] == kMouse);
    }
    // on the 65C02 an enhanced iie and a iic find it; a iie on the nmos chip does not
    s.cpu = tisple::Cpu::k65c02;
    s.model = tisple::MachineModel::kIIeEnhanced;
    if (Detect(results, b, s, f)) {
        CHECK(results, f.model == kIIeEnhanced && f.cmos && f.mousetext);
    }
    s.model = tisple::MachineModel::kIIc;
    if (Detect(results, b, s, f)) {
        CHECK(results, f.model == kIIc && f.cmos);
    }
    s.cpu = tisple::ModelCpu(tisple::MachineModel::kIIe);
    s.model = tisple::MachineModel::kIIe;
    if (Detect(results, b, s, f)) {
        CHECK(results, f.model == kIIe && !f.cmos);
    }
    // a 64 KB iie: no card answers in aux memory
    s.model = tisple::MachineModel::kIIeEnhanced;
    s.aux_ram = false;
    if (Detect(results, b, s, f)) {
        CHECK(results, f.model == kIIeEnhanced && f.kb == 64 && !f.aux && !f.col80);
        CHECK(results, f.card[3] == kNone);
    }
}

void TestCards(TestResults& results, const LibBuild& b) {
    Setup s;
    s.mouse = false;
    s.mockingboard = 4;
    s.disk2 = true;
    Found f;
    if (Detect(results, b, s, f)) {
        CHECK(results, f.card[4] == kMockingboard && f.card[6] == kDisk2 && f.mouse_slot == 0);
    }
    s.mockingboard = 2;
    s.mouse = true;
    if (Detect(results, b, s, f)) {
        CHECK(results, f.card[2] == kMockingboard && f.card[4] == kMouse && f.card[6] == kDisk2);
    }
}

// Apple's own roms, when the owner has them: their identification bytes and $FE1F
void TestAppleRoms(TestResults& results, const LibBuild& b) {
    struct Rom {
        const char* file;
        int model;
        int kb;
        bool cmos;
    };
    // each on the cpu its model has (RomModel, ModelCpu), as tisple machine --rom runs it
    const Rom roms[] = {
        {"Apple2e.rom", kIIe, 128, false},
        {"Apple2e_Enhanced.rom", kIIeEnhanced, 128, true},
        {"Apple2_Plus.rom", kIIPlus, 48, false},
    };
    int tried = 0;
    for (const Rom& r : roms) {
        Setup s;
        s.rom = r.file;
        std::vector<uint8_t> bytes;
        if (!ReadRomFile(r.file, bytes)) {
            continue;
        }
        std::unique_ptr<tisple::Apple2> loaded = tisple::NewApple2();
        std::string error;
        tisple::MachineModel model = tisple::MachineModel::kIIgs;
        CHECK(results, tisple::LoadRom(*loaded, bytes.data(), bytes.size(), error) &&
                           tisple::RomModel(*loaded, model));
        s.cpu = tisple::ModelCpu(model);
        CHECK(results, (s.cpu == tisple::Cpu::k65c02) == r.cmos);
        Found f;
        if (Detect(results, b, s, f)) {
            CHECK(results, f.model == r.model && f.kb == r.kb && f.cmos == r.cmos);
            tried++;
        }
    }
    std::printf("machine: detected through %d of Apple's roms\n", tried);
}

// detect called from the language card, in bank 2 and in bank 1, comes back to it with the
// card's ram in, under Apple's rom and without one
void TestFromCard(TestResults& results) {
    std::string text = std::string("(let fill_a (array u8 0x5800) :address 0x0800 :reserve)\n"
                                   "(let fill_b (array u8 0x3300) :address 0x6200 :reserve)\n") +
                       kDetect;
    for (int bank1 = 0; bank1 < 2; bank1++) {
        tisple::BuildOptions options;
        options.bank1_test = bank1;
        options.no_inline = {"machine.detect"};
        LibBuild b;
        // bank 2: main memory filled so the code goes to the card; bank 1: the test option
        if (!BuildLibProgram(results, bank1 == 1 ? std::string(kDetect) : text, options, b)) {
            return;
        }
        int32_t detect = tisple::FindFunction(b.program, "machine.detect");
        uint16_t at = detect < 0 ? 0 : b.code.function_address[static_cast<size_t>(detect)];
        // a function with a jsr never goes to bank 1 (card_bank.cpp), so from bank 1 code
        // detect is reached through a stub that switches bank 2 in
        int32_t main = b.program.main;
        bool detect_in_bank1 = false;
        bool main_in_bank1 = false;
        for (const tisple::FunctionSpan& span : b.code.spans) {
            detect_in_bank1 = detect_in_bank1 || (span.function == detect && span.card_bank1);
            main_in_bank1 = main_in_bank1 || (span.function == main && span.card_bank1);
        }
        CHECK(results, bank1 == 1 ? main_in_bank1 && !detect_in_bank1 : at >= 0xD000);
        Setup s;
        Found f;
        if (Detect(results, b, s, f)) {
            CHECK(results, f.model == kIIeEnhanced && f.kb == 128);
        }
        s.rom = "Apple2e.rom";
        if (Detect(results, b, s, f)) {
            CHECK(results, f.model == kIIe && f.kb == 128);
        }
        s.rom = "Apple2_Plus.rom";
        if (Detect(results, b, s, f)) {
            CHECK(results, f.model == kIIPlus);
        }
    }
}

} // namespace

void RunMachineLibTests(TestResults& results) {
    LibBuild b;
    if (BuildLibProgram(results, kDetect, tisple::BuildOptions(), b)) {
        TestModels(results, b);
        TestCards(results, b);
        TestAppleRoms(results, b);
    }
    TestFromCard(results);
}

} // namespace tisple_tests
