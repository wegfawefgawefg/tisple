// the extended 80-column card's memory: RAMRD, RAMWRT, ALTZP and 80STORE (with PAGE2 and
// HIRES) in every combination, the language card in both banks of both rams, the status
// reads, and snapshots that carry auxiliary memory

#include "test_machine.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace tisple_tests {

namespace {

constexpr uint8_t kMainMark = 0x11;
constexpr uint8_t kAuxMark = 0xAA;
constexpr uint8_t kStored = 0x5C;

struct Probe {
    uint16_t addr = 0;
    const char* what = "";
};

// one address in each part of memory the switches treat differently
const Probe kProbes[] = {
    {0x0010, "zero page"},  {0x0150, "stack"},         {0x0300, "page 3"},
    {0x0480, "text page"},  {0x2100, "hires page"},    {0x5000, "plain ram"},
    {0xBF00, "top of ram"}, {0xD000, "language card"},
};

struct Switches {
    bool ramrd = false;
    bool ramwrt = false;
    bool altzp = false;
    bool store80 = false;
    bool page2 = false;
    bool hires = false;
};

// the rules as the IIe Technical Reference states them, written out on their own
bool ExpectAux(const Switches& s, uint16_t addr, bool is_write) {
    if (addr < 0x0200 || addr >= 0xD000) {
        return s.altzp;
    }
    bool text = addr >= 0x0400 && addr < 0x0800;
    bool hires = addr >= 0x2000 && addr < 0x4000;
    if (s.store80 && (text || (hires && s.hires))) {
        return s.page2;
    }
    return is_write ? s.ramwrt : s.ramrd;
}

void Emit(std::vector<uint8_t>& code, uint8_t op, uint16_t addr) {
    code.push_back(op);
    code.push_back(static_cast<uint8_t>(addr & 0xFF));
    code.push_back(static_cast<uint8_t>(addr >> 8));
}

// switches on, a read of each probe, then a store to each; the code sits at $6000 in both
// rams so RAMRD can't pull it away
std::vector<uint8_t> ProbeProgram(const Switches& s) {
    std::vector<uint8_t> code;
    Emit(code, 0xAD, 0xC08B); // LDA $C08B twice: bank 1, ram reads, writes on
    Emit(code, 0xAD, 0xC08B);
    Emit(code, 0xAD, 0xC083); // and twice more for bank 2
    Emit(code, 0xAD, 0xC083);
    if (s.ramrd) {
        Emit(code, 0x8D, 0xC003);
    }
    if (s.ramwrt) {
        Emit(code, 0x8D, 0xC005);
    }
    if (s.altzp) {
        Emit(code, 0x8D, 0xC009);
    }
    if (s.store80) {
        Emit(code, 0x8D, 0xC001);
    }
    if (s.page2) {
        Emit(code, 0xAD, 0xC055);
    }
    if (s.hires) {
        Emit(code, 0xAD, 0xC057);
    }
    for (const Probe& p : kProbes) {
        Emit(code, 0xAD, p.addr); // LDA probe
    }
    code.push_back(0xA9); // LDA #kStored
    code.push_back(kStored);
    for (const Probe& p : kProbes) {
        Emit(code, 0x8D, p.addr); // STA probe
    }
    code.push_back(0x00);
    return code;
}

uint8_t* MainByte(tisple::Apple2& a, uint16_t addr) {
    return addr >= 0xD000 ? &a.lc.bank2_d000[addr - 0xD000] : &a.cpu.mem[addr];
}

uint8_t* AuxByte(tisple::Apple2& a, uint16_t addr) {
    return addr >= 0xD000 ? &a.lc.aux_bank2_d000[addr - 0xD000] : &a.aux[addr];
}

// the value each probe's read saw, from the bus log; -1 if it was not read
int ReadValue(const std::vector<tisple::BusCycle>& log, uint16_t addr) {
    for (const tisple::BusCycle& c : log) {
        if (c.addr == addr && !c.is_write) {
            return c.value;
        }
    }
    return -1;
}

bool RunCombination(const Switches& s, std::string& failure) {
    std::vector<uint8_t> code = ProbeProgram(s);
    auto a = MachineWithBytes(code);
    std::memcpy(a->aux + 0x6000, code.data(), code.size());
    for (const Probe& p : kProbes) {
        *MainByte(*a, p.addr) = kMainMark;
        *AuxByte(*a, p.addr) = kAuxMark;
    }
    std::vector<tisple::BusCycle> log;
    tisple::SetBusLog(a->cpu, &log);
    RunToStop(*a);
    tisple::SetBusLog(a->cpu, nullptr);
    if (a->stop_reason != tisple::MachineStop::kBrk) {
        failure = "did not run to its BRK";
        return false;
    }
    for (const Probe& p : kProbes) {
        bool read_aux = ExpectAux(s, p.addr, false);
        bool write_aux = ExpectAux(s, p.addr, true);
        int seen = ReadValue(log, p.addr);
        uint8_t main_after = *MainByte(*a, p.addr);
        uint8_t aux_after = *AuxByte(*a, p.addr);
        bool read_ok = seen == (read_aux ? kAuxMark : kMainMark);
        bool write_ok = write_aux ? aux_after == kStored && main_after == kMainMark
                                  : main_after == kStored && aux_after == kAuxMark;
        if (!read_ok || !write_ok) {
            failure = std::string(p.what) + (read_ok ? ": write" : ": read") + " went to the " +
                      "wrong ram";
            return false;
        }
    }
    return true;
}

void TestEveryCombination(TestResults& results) {
    int failures = 0;
    for (int bits = 0; bits < 64; bits++) {
        Switches s;
        s.ramrd = (bits & 1) != 0;
        s.ramwrt = (bits & 2) != 0;
        s.altzp = (bits & 4) != 0;
        s.store80 = (bits & 8) != 0;
        s.page2 = (bits & 16) != 0;
        s.hires = (bits & 32) != 0;
        std::string failure;
        if (!RunCombination(s, failure)) {
            failures++;
            std::fprintf(stderr,
                         "aux: ramrd %d ramwrt %d altzp %d 80store %d page2 %d hires %d: %s\n",
                         s.ramrd, s.ramwrt, s.altzp, s.store80, s.page2, s.hires, failure.c_str());
        }
    }
    CHECK(results, failures == 0);
}

// $C013-$C018 show the memory switches in bit 7; $C01C and $C01F page 2 and 80 columns
void TestStatusReads(TestResults& results) {
    std::vector<uint8_t> code;
    Emit(code, 0x8D, 0xC003); // RAMRD
    Emit(code, 0x8D, 0xC005); // RAMWRT
    Emit(code, 0x8D, 0xC009); // ALTZP
    Emit(code, 0x8D, 0xC001); // 80STORE
    Emit(code, 0x8D, 0xC00D); // 80COL
    for (uint16_t addr = 0xC013; addr <= 0xC01F; addr++) {
        Emit(code, 0xAD, addr);
    }
    code.push_back(0x00);
    auto a = MachineWithBytes(code);
    std::memcpy(a->aux + 0x6000, code.data(), code.size());
    std::vector<tisple::BusCycle> log;
    tisple::SetBusLog(a->cpu, &log);
    RunToStop(*a);
    tisple::SetBusLog(a->cpu, nullptr);
    auto bit7 = [&](uint16_t addr) { return (ReadValue(log, addr) & 0x80) != 0; };
    CHECK(results, bit7(0xC013) && bit7(0xC014)); // RDRAMRD, RDRAMWRT
    CHECK(results, !bit7(0xC015));                // RDCXROM
    CHECK(results, bit7(0xC016));                 // RDALTZP
    CHECK(results, !bit7(0xC017));                // RDC3ROM
    CHECK(results, bit7(0xC018));                 // RD80STORE
    CHECK(results, !bit7(0xC01C));                // RDPAGE2
    CHECK(results, bit7(0xC01F));                 // RD80COL
}

// ALTZP picks the language card too, in both $D000 banks; the bank switches stay shared
void TestLanguageCardBanks(TestResults& results) {
    std::vector<uint8_t> code;
    Emit(code, 0xAD, 0xC08B); // bank 1, ram, writes on
    Emit(code, 0xAD, 0xC08B);
    code.insert(code.end(), {0xA9, 0x01}); // LDA #1
    Emit(code, 0x8D, 0xD000);              // main bank 1
    Emit(code, 0x8D, 0xC009);              // ALTZP on
    code.insert(code.end(), {0xA9, 0x02});
    Emit(code, 0x8D, 0xD000); // aux bank 1
    Emit(code, 0x8D, 0xE000); // aux $E000
    Emit(code, 0xAD, 0xC083); // bank 2
    Emit(code, 0xAD, 0xC083);
    code.insert(code.end(), {0xA9, 0x03});
    Emit(code, 0x8D, 0xD000); // aux bank 2
    Emit(code, 0x8D, 0xC008); // ALTZP off
    code.insert(code.end(), {0xA9, 0x04});
    Emit(code, 0x8D, 0xD000); // main bank 2
    Emit(code, 0x8D, 0xE000); // main $E000
    code.push_back(0x00);
    auto a = MachineWithBytes(code);
    RunToStop(*a);
    CHECK(results, a->lc.bank1_d000[0] == 1);
    CHECK(results, a->lc.aux_bank1_d000[0] == 2);
    CHECK(results, a->lc.aux_e000[0] == 2);
    CHECK(results, a->lc.aux_bank2_d000[0] == 3);
    CHECK(results, a->lc.bank2_d000[0] == 4);
    CHECK(results, a->lc.e000[0] == 4);
    CHECK(results, tisple::PeekAux(*a, 0xD000) == 3 && tisple::PeekAux(*a, 0xC000) == 2);
}

// a snapshot holds auxiliary ram and its language card; the switches come back too
void TestSnapshotAux(TestResults& results) {
    auto a = MachineWithBytes({0x8D, 0x05, 0xC0, 0xA9, 0x77, 0x8D, 0x00, 0x50, 0x00});
    RunToStop(*a);
    CHECK(results, a->aux[0x5000] == 0x77 && a->cpu.mem[0x5000] == 0);
    a->lc.aux_e000[5] = 0x42;
    std::vector<uint8_t> snapshot = tisple::SaveSnapshot(*a);
    std::unique_ptr<tisple::Apple2> b = tisple::NewApple2();
    std::string error;
    CHECK(results, tisple::LoadSnapshot(*b, snapshot.data(), snapshot.size(), error));
    CHECK(results, b->aux[0x5000] == 0x77 && b->lc.aux_e000[5] == 0x42);
    CHECK(results, b->sw.ramwrt && b->cpu.io.base == 0);
    CHECK(results, tisple::PeekMemory(*b, 0x5000) == 0); // reads still go to main
}

// segments load into their own memory before the program starts
void TestLoadSegments(TestResults& results) {
    auto a = MachineWithBytes({0x00});
    std::string error;
    tisple::MemorySegment aux;
    aux.bank = tisple::MemoryBank::kAux;
    aux.address = 0x0800;
    aux.bytes = {1, 2, 3};
    tisple::MemorySegment card;
    card.bank = tisple::MemoryBank::kCard;
    card.address = 0xDFFF;
    card.bytes = {4, 5};
    CHECK(results, tisple::LoadSegment(*a, aux, error) && tisple::LoadSegment(*a, card, error));
    CHECK(results, a->aux[0x0802] == 3 && a->cpu.mem[0x0802] == 0);
    CHECK(results, a->lc.bank2_d000[0x0FFF] == 4 && a->lc.e000[0] == 5);
    card.address = 0xC000;
    CHECK(results, !tisple::LoadSegment(*a, card, error));
}

} // namespace

void RunMachineAuxTests(TestResults& results) {
    TestEveryCombination(results);
    TestStatusReads(results);
    TestLanguageCardBanks(results);
    TestSnapshotAux(results);
    TestLoadSegments(results);
}

} // namespace tisple_tests
