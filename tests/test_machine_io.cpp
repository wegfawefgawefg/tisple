// soft-switch semantics of the built-in apple iie: what counts as an access, the keyboard
// strobe, paddle timers, the language card, $C019 timing and the floating bus

#include "test_machine.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

using tisple::Apple2;

uint64_t TogglesFor(const std::vector<uint8_t>& bytes) {
    auto a = MachineWithBytes(bytes);
    RunToStop(*a);
    return a->speaker.toggle_count;
}

// any read or write is an access; dummy reads and read-modify-write cycles count too
void TestSpeakerAccesses(TestResults& results) {
    CHECK(results, TogglesFor({0xAD, 0x30, 0xC0, 0x00}) == 1);             // LDA $C030
    CHECK(results, TogglesFor({0x8D, 0x30, 0xC0, 0x00}) == 1);             // STA $C030
    CHECK(results, TogglesFor({0x2C, 0x30, 0xC0, 0x00}) == 1);             // BIT $C030
    CHECK(results, TogglesFor({0xA2, 0x00, 0x9D, 0x30, 0xC0, 0x00}) == 2); // STA $C030,X
    CHECK(results, TogglesFor({0xEE, 0x30, 0xC0, 0x00}) == 3);             // INC $C030
    CHECK(results, TogglesFor({0xAD, 0x3F, 0xC0, 0x00}) == 1);             // $C03F mirrors

    // the toggle is logged at its bus cycle: LDA abs reads on its fourth cycle
    auto a = MachineWithBytes({0xEA, 0xAD, 0x30, 0xC0, 0x8D, 0x30, 0xC0, 0x00});
    RunToStop(*a);
    CHECK(results, a->speaker.toggle_count == 2);
    CHECK(results, tisple::SpeakerToggleCycle(*a, 0) == 2 + 3);
    CHECK(results, tisple::SpeakerToggleCycle(*a, 1) == 6 + 3);
}

void TestDisplaySwitches(TestResults& results) {
    // reads flip display switches as writes do
    auto a = MachineWithBytes({0xAD, 0x50, 0xC0,   // LDA $C050: graphics
                               0x8D, 0x53, 0xC0,   // STA $C053: mixed
                               0xAD, 0x55, 0xC0,   // LDA $C055: page 2
                               0x8D, 0x57, 0xC0,   // STA $C057: hi-res
                               0xAD, 0x59, 0xC0,   // LDA $C059: an0 on
                               0xAD, 0x1A, 0xC0,   // LDA $C01A: RDTEXT
                               0x85, 0x00,         // STA $00
                               0xAD, 0x1D, 0xC0,   // LDA $C01D: RDHIRES
                               0x85, 0x01, 0x00}); // STA $01
    RunToStop(*a);
    CHECK(results, !a->sw.text);
    CHECK(results, a->sw.mixed);
    CHECK(results, a->sw.page2);
    CHECK(results, a->sw.hires);
    CHECK(results, a->sw.an[0]);
    CHECK(results, (a->cpu.mem[0x00] & 0x80) == 0);
    CHECK(results, (a->cpu.mem[0x01] & 0x80) != 0);
}

// $C000-$C00F are write-only switches: reading them returns the keyboard
void TestMemorySwitchesWriteOnly(TestResults& results) {
    auto a = MachineWithBytes({0xAD, 0x01, 0xC0, // LDA $C001: no effect
                               0xAD, 0x18, 0xC0, // LDA $C018: RD80STORE
                               0x85, 0x00,       // STA $00
                               0x8D, 0x01, 0xC0, // STA $C001: 80store on
                               0x8D, 0x0F, 0xC0, // STA $C00F: altchar on
                               0xAD, 0x18, 0xC0, 0x85, 0x01, 0xAD, 0x1E, 0xC0, 0x85, 0x02, 0x00});
    RunToStop(*a);
    CHECK(results, (a->cpu.mem[0x00] & 0x80) == 0);
    CHECK(results, (a->cpu.mem[0x01] & 0x80) != 0);
    CHECK(results, (a->cpu.mem[0x02] & 0x80) != 0);
    CHECK(results, a->sw.store80 && a->sw.altchar);

    // aux memory is a stub: tracked and noted, main memory still used
    auto b = MachineWithBytes({0x8D, 0x05, 0xC0, 0xA9, 0x77, 0x85, 0x10, 0x00});
    RunToStop(*b);
    CHECK(results, b->sw.ramwrt);
    CHECK(results, b->cpu.mem[0x10] == 0x77);
    CHECK(results, (b->stub_notes & tisple::kNoteAuxMemory) != 0);
}

void TestKeyboardStrobe(TestResults& results) {
    auto a = MachineWithBytes({0xAD, 0x00, 0xC0, 0x85, 0x00, // LDA $C000
                               0xAD, 0x00, 0xC0, 0x85, 0x01, // again: reading keeps it
                               0xAD, 0x11, 0xC0, 0x85, 0x02, // $C011 status: keeps it
                               0xAD, 0x05, 0xC0, 0x85, 0x03, // $C005 mirrors $C000
                               0xAD, 0x10, 0xC0, 0x85, 0x04, // $C010: clears, bit 7 = key down
                               0xAD, 0x00, 0xC0, 0x85, 0x05, 0x00});
    tisple::PressKey(*a, 'A');
    RunToStop(*a);
    CHECK(results, a->cpu.mem[0x00] == 0xC1);
    CHECK(results, a->cpu.mem[0x01] == 0xC1);
    CHECK(results, (a->cpu.mem[0x02] & 0x7F) == 0x41);
    CHECK(results, a->cpu.mem[0x03] == 0xC1);
    CHECK(results, a->cpu.mem[0x04] == 0xC1);
    CHECK(results, a->cpu.mem[0x05] == 0x41);

    // a write anywhere in $C010-$C01F clears the strobe too; $C010 bit 7 is any-key-down
    auto b = MachineWithBytes({0x8D, 0x1F, 0xC0,             // STA $C01F
                               0xAD, 0x00, 0xC0, 0x85, 0x00, // LDA $C000
                               0xAD, 0x10, 0xC0, 0x85, 0x01, 0x00});
    tisple::PressKey(*b, 'B');
    tisple::ReleaseKey(*b);
    RunToStop(*b);
    CHECK(results, b->cpu.mem[0x00] == 0x42);
    CHECK(results, b->cpu.mem[0x01] == 0x42);
}

// a PREAD-style loop: 11 cycles per count, as the rom's paddle routine
std::vector<uint8_t> PaddleLoop() {
    return {0x8D, 0x70, 0xC0, // STA $C070: write at cycle 3
            0xA2, 0x00,       // LDX #0
            0xAD, 0x64, 0xC0, // loop: LDA $C064
            0x10, 0x04,       // BPL done
            0xE8,             // INX
            0xD0, 0xF8,       // BNE loop
            0x00,             // (wrapped)
            0x86, 0x00,       // done: STX $00
            0x00};
}

void TestPaddleTiming(TestResults& results) {
    // a pass is 4 + 2 + 2 + 3 = 11 cycles; reads land
    // at 9 + 11k. paddle 100 times out 1 + 100 * 2816 / 255 = 1105 cycles after the trigger
    auto a = MachineWithBytes(PaddleLoop());
    tisple::SetPaddle(*a, 0, 100);
    RunToStop(*a);
    CHECK(results, a->game.paddle_end[0] == 3 + 1 + 1104);
    CHECK(results, a->cpu.mem[0x00] == 100);

    auto zero = MachineWithBytes(PaddleLoop());
    tisple::SetPaddle(*zero, 0, 0);
    RunToStop(*zero);
    CHECK(results, zero->cpu.mem[0x00] == 0);

    auto centre = MachineWithBytes(PaddleLoop());
    RunToStop(*centre);
    CHECK(results, centre->cpu.mem[0x00] >= 126 && centre->cpu.mem[0x00] <= 128);

    // buttons: bit 7, and $C069 mirrors $C061
    auto b = MachineWithBytes({0xAD, 0x61, 0xC0, 0x85, 0x00, 0xAD, 0x69, 0xC0, 0x85, 0x01, 0xAD,
                               0x62, 0xC0, 0x85, 0x02, 0x00});
    tisple::SetButton(*b, 0, true);
    RunToStop(*b);
    CHECK(results, (b->cpu.mem[0x00] & 0x80) != 0);
    CHECK(results, (b->cpu.mem[0x01] & 0x80) != 0);
    CHECK(results, (b->cpu.mem[0x02] & 0x80) == 0);
}

void TestLanguageCard(TestResults& results) {
    // two reads of $C083 enable writes; bank 1 and bank 2 are separate; $C082 shows rom
    auto a =
        MachineWithBytes({0xAD, 0x83, 0xC0, 0xAD, 0x83, 0xC0,             // bank 2 ram, write on
                          0xA9, 0x42, 0x8D, 0x00, 0xD0,                   // STA $D000
                          0xAD, 0x00, 0xD0, 0x85, 0x00,                   // LDA $D000
                          0xAD, 0x8B, 0xC0, 0xAD, 0x8B, 0xC0,             // bank 1
                          0xA9, 0x24, 0x8D, 0x00, 0xD0,                   // STA $D000
                          0xAD, 0x00, 0xD0, 0x85, 0x01, 0xAD, 0x80, 0xC0, // bank 2, write off
                          0xAD, 0x00, 0xD0, 0x85, 0x02, 0xA9, 0x99, 0x8D, 0x00, 0xE0, // ignored
                          0xAD, 0x00, 0xE0, 0x85, 0x03, 0xAD, 0x82, 0xC0,             // rom
                          0xAD, 0x00, 0xD0, 0x85, 0x04, 0x00});
    RunToStop(*a);
    CHECK(results, a->cpu.mem[0x00] == 0x42);
    CHECK(results, a->cpu.mem[0x01] == 0x24);
    CHECK(results, a->cpu.mem[0x02] == 0x42);
    CHECK(results, a->cpu.mem[0x03] == 0x00);
    CHECK(results, a->cpu.mem[0x04] == 0xD0); // no rom: the address's high byte

    // one read is not enough, and a write in between resets the pre-write state
    auto b = MachineWithBytes({0xAD, 0x83, 0xC0,             // LDA $C083
                               0xA9, 0x55, 0x8D, 0x00, 0xD0, // STA $D000: ignored
                               0x8D, 0x83, 0xC0,             // STA $C083
                               0xAD, 0x83, 0xC0,             // LDA $C083
                               0xA9, 0x66, 0x8D, 0x01, 0xD0, // STA $D001: ignored
                               0xAD, 0x00, 0xD0, 0x85, 0x00, 0xAD, 0x01, 0xD0, 0x85, 0x01, 0x00});
    RunToStop(*b);
    CHECK(results, b->cpu.mem[0x00] == 0x00);
    CHECK(results, b->cpu.mem[0x01] == 0x00);
    CHECK(results, b->lc.read_ram && !b->lc.write_enable);
}

// wait for vertical blank: BIT $00 or NOP first, so the 7-cycle polling loop lands on
// either side of cycle 12480, where RDVBLBAR goes low
void TestVblankTiming(TestResults& results) {
    // reads at 6 + 7k: one lands exactly on 12480 and sees blanking
    auto on_edge = MachineWithBytes({0x24, 0x00, 0xAD, 0x19, 0xC0, 0x30, 0xFB, 0x00});
    tisple::RunOutcome hit = RunToStop(*on_edge);
    CHECK(results, hit.cycles == 12480 + 3);

    // reads at 5 + 7k: 12479 still sees the display, 12486 sees blanking
    auto before = MachineWithBytes({0xEA, 0xAD, 0x19, 0xC0, 0x30, 0xFB, 0x00});
    tisple::RunOutcome late = RunToStop(*before);
    CHECK(results, late.cycles == 12486 + 3);

    // and back up at the next frame: reads at 12486 + 7k; 17029 is still blank
    auto end = MachineWithBytes({0x24, 0x00, 0xAD, 0x19, 0xC0, 0x30, 0xFB, // wait for blank
                                 0xAD, 0x19, 0xC0, 0x10, 0xFB, 0x00});     // wait for display
    tisple::RunOutcome back = RunToStop(*end);
    CHECK(results, back.cycles == 17036 + 3);

    CHECK(results, !tisple::InVblank(12479));
    CHECK(results, tisple::InVblank(12480));
    CHECK(results, tisple::InVblank(17029));
    CHECK(results, !tisple::InVblank(17030));
}

// the scanner address formula against the plain screen layout, and AppleWin's tables
void TestScannerAddress(TestResults& results) {
    tisple::SoftSwitches text;
    tisple::SoftSwitches hires;
    hires.text = false;
    hires.hires = true;
    tisple::SoftSwitches page2 = hires;
    page2.page2 = true;
    bool all_match = true;
    for (uint32_t line = 0; line < tisple::kVisibleLines; line++) {
        uint32_t row = line / 8;
        uint32_t row_offset = (row & 7) * 0x80 + (row >> 3) * 0x28;
        for (uint32_t col = 0; col < tisple::kVisibleColumns; col++) {
            uint64_t cycle = line * tisple::kCyclesPerLine + tisple::kFirstVisibleCycle + col;
            uint32_t text_addr = 0x400 + row_offset + col;
            uint32_t hires_addr = 0x2000 + (line & 7) * 0x400 + row_offset + col;
            all_match = all_match && tisple::ScannerAddress(text, cycle) == text_addr;
            all_match = all_match && tisple::ScannerAddress(hires, cycle) == hires_addr;
            all_match = all_match && tisple::ScannerAddress(page2, cycle) == hires_addr + 0x2000;
        }
    }
    CHECK(results, all_match);
    // horizontal blank of line 0, from kAPPLE_IIE_HORZ_CLOCK_OFFSET: $68, $68, $69, ...
    CHECK(results, tisple::ScannerAddress(text, 0) == 0x0468);
    CHECK(results, tisple::ScannerAddress(text, 1) == 0x0468);
    CHECK(results, tisple::ScannerAddress(text, 2) == 0x0469);
    CHECK(results, tisple::ScannerAddress(text, 24) == 0x047F);
    // rows 8-15 use the second table row: $10 at the start of the line
    CHECK(results, tisple::ScannerAddress(text, 64 * tisple::kCyclesPerLine) == 0x0410);

    // a read of an i/o address with no status bit returns what the scanner fetched
    auto a = MachineWithBytes({0xEA, 0xEA, 0xAD, 0x20, 0xC0, 0x85, 0x00, 0x00});
    for (int i = 0; i < 0x400; i++) {
        a->cpu.mem[0x400 + i] = static_cast<uint8_t>(i * 7 + 3);
    }
    RunToStop(*a);
    CHECK(results, a->cpu.mem[0x00] == a->cpu.mem[tisple::ScannerAddress(a->sw, 4 + 3)]);
}

} // namespace

void RunMachineIoTests(TestResults& results) {
    TestSpeakerAccesses(results);
    TestDisplaySwitches(results);
    TestMemorySwitchesWriteOnly(results);
    TestKeyboardStrobe(results);
    TestPaddleTiming(results);
    TestLanguageCard(results);
    TestVblankTiming(results);
    TestScannerAddress(results);
}

} // namespace tisple_tests
