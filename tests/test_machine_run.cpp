// running the built-in apple iie: stops, roms, slot cards, snapshots, keys, speaker

#include "machine/png.hpp"
#include "machine/slots.hpp"
#include "machine/speaker_audio.hpp"
#include "test_machine.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

namespace tisple_tests {

std::unique_ptr<tisple::Apple2> MachineWithBytes(const std::vector<uint8_t>& bytes) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    std::string error;
    tisple::LoadProgram(*a, bytes.data(), bytes.size(), tisple::kDefaultOrigin, error);
    return a;
}

std::unique_ptr<tisple::Apple2> MachineWithFile(const char* name) {
    std::string path = std::string(TISPLE_SOURCE_DIR) + "/tests/programs/" + name;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "missing %s\n", path.c_str());
        return nullptr;
    }
    std::vector<uint8_t> bytes;
    int c = 0;
    while ((c = std::fgetc(file)) != EOF) {
        bytes.push_back(static_cast<uint8_t>(c));
    }
    std::fclose(file);
    return MachineWithBytes(bytes);
}

tisple::RunOutcome RunToStop(tisple::Apple2& a) {
    return tisple::RunApple2(a, 50 * tisple::kCyclesPerFrame);
}

namespace {

using tisple::MachineStop;

void TestStops(TestResults& results) {
    auto brk = MachineWithBytes({0xEA, 0xEA, 0x00});
    tisple::RunOutcome r = RunToStop(*brk);
    CHECK(results, r.reason == MachineStop::kBrk);
    CHECK(results, brk->stop_pc == 0x6002);
    CHECK(results, r.cycles == 4);

    // the entry's RTS returns to the stop address we pushed
    auto rts = MachineWithBytes({0xA9, 0x01, 0x60});
    r = RunToStop(*rts);
    CHECK(results, r.reason == MachineStop::kReturned);
    CHECK(results, r.cycles == 2 + 6);
    CHECK(results, tisple::GetRegs(rts->cpu).s == 0xFF);

    // JSR $FDED with no rom loaded
    auto rom = MachineWithBytes({0x20, 0xED, 0xFD});
    r = RunToStop(*rom);
    CHECK(results, r.reason == MachineStop::kNoRom);
    CHECK(results, rom->stop_pc == 0xFDED);
    CHECK(results, tisple::DescribeStop(*rom) == "jumped into ROM at $FDED (no rom loaded)");

    auto slot = MachineWithBytes({0x4C, 0x00, 0xC6});
    r = RunToStop(*slot);
    CHECK(results, r.reason == MachineStop::kNoRom && slot->stop_pc == 0xC600);

    auto illegal = MachineWithBytes({0xEA, 0x02});
    r = RunToStop(*illegal);
    CHECK(results, r.reason == MachineStop::kIllegalOpcode && illegal->stop_pc == 0x6001);

    // no rom: $D000-$FFFF read as a pattern and ignore writes
    auto pattern =
        MachineWithBytes({0xA9, 0x11, 0x8D, 0x34, 0xF8, 0xAD, 0x34, 0xF8, 0x85, 0x00, 0x00});
    RunToStop(*pattern);
    CHECK(results, pattern->cpu.mem[0x00] == 0xF8);

    // a stopped machine keeps its clock and screen running
    uint64_t before = tisple::Cycles(*brk);
    r = tisple::RunApple2(*brk, 1000);
    CHECK(results, r.reason == MachineStop::kBrk && tisple::Cycles(*brk) == before + 1000);

    // a budget that runs out leaves the machine ready to go on
    auto spin = MachineWithBytes({0x4C, 0x00, 0x60});
    r = tisple::RunApple2(*spin, 100);
    CHECK(results, r.reason == MachineStop::kRunning && r.cycles >= 100 && r.cycles < 103);
}

// a tiny hand-written rom: reset jumps to $D000, which stores a mark and spins
void TestRomBoot(TestResults& results) {
    std::vector<uint8_t> rom(0x3000, 0xEA);
    const uint8_t code[] = {0xA9, 0x5A, 0x8D, 0x00, 0x03, 0x4C, 0x05, 0xD0};
    for (size_t i = 0; i < sizeof(code); i++) {
        rom[i] = code[i];
    }
    rom[0x2FFC] = 0x00; // reset vector $D000
    rom[0x2FFD] = 0xD0;
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    std::string error;
    CHECK(results, !tisple::LoadRom(*a, rom.data(), 100, error));
    CHECK(results, tisple::LoadRom(*a, rom.data(), rom.size(), error));
    tisple::ResetApple2(*a);
    CHECK(results, tisple::GetRegs(a->cpu).pc == 0xD000);
    CHECK(results, tisple::Cycles(*a) == 7);
    tisple::RunOutcome r = tisple::RunApple2(*a, 1000);
    CHECK(results, r.reason == MachineStop::kRunning);
    CHECK(results, a->cpu.mem[0x0300] == 0x5A);
    CHECK(results, tisple::PeekMemory(*a, 0xD001) == 0x5A);

    // a direct-loaded program can call into a loaded rom
    auto b = MachineWithBytes({0x20, 0x00, 0xD0});
    CHECK(results, tisple::LoadRom(*b, rom.data(), rom.size(), error));
    r = tisple::RunApple2(*b, 1000);
    CHECK(results, r.reason == MachineStop::kRunning && b->cpu.mem[0x0300] == 0x5A);
}

void TestSlotCard(TestResults& results) {
    // JSR $C600 runs the card's rom (an RTS); $C0E0 is the card's first register
    auto a = MachineWithBytes({0x20, 0x00, 0xC6,                         // JSR $C600
                               0xAD, 0xE0, 0xC0,                         // LDA $C0E0
                               0x85, 0x00, 0xA9, 0x31, 0x8D, 0xE1, 0xC0, // STA $C0E1
                               0xAD, 0x00, 0xC8, 0x85, 0x01, // LDA $C800: card's expansion
                               0xAD, 0xFF, 0xCF,             // LDA $CFFF: release
                               0xAD, 0x00, 0xC8, 0x85, 0x02, 0x00});
    tisple::InsertCard(*a, 6, tisple::CardKind::kTestCard);
    a->slots.test_card.rom[0] = 0x60;
    a->slots.test_card.regs[0] = 0x77;
    a->slots.test_card.expansion[0] = 0x99;
    a->slots.test_card.expansion[0x7FF] = 0x98;
    tisple::RunOutcome r = RunToStop(*a);
    CHECK(results, r.reason == MachineStop::kBrk);
    CHECK(results, a->cpu.mem[0x00] == 0x77);
    CHECK(results, a->slots.test_card.regs[1] == 0x31);
    CHECK(results, a->cpu.mem[0x01] == 0x99);
    CHECK(results, a->slots.expansion_slot == 0);
    CHECK(results, tisple::PeekMemory(*a, 0xC800) == 0x00); // released: nothing there
    CHECK(results, tisple::PeekMemory(*a, 0xC600) == 0x60);
}

void TestScriptedKeys(TestResults& results) {
    auto a = MachineWithFile("machine_keys.bin");
    if (a == nullptr) {
        CHECK(results, false);
        return;
    }
    CHECK(results, tisple::ScheduleKey(*a, 5000, 'B'));
    CHECK(results, tisple::ScheduleKey(*a, 1000, 'A')); // out of order is fine
    tisple::RunOutcome r = RunToStop(*a);
    CHECK(results, r.reason == MachineStop::kBrk);
    CHECK(results, a->cpu.mem[0x0300] == 0xC1);
    CHECK(results, a->cpu.mem[0x0301] == 0xC2);
    CHECK(results, a->cpu.mem[0x0302] == 0x42);
    CHECK(results, r.cycles > 5000 && r.cycles < 5100);
}

void TestSpeakerWav(TestResults& results) {
    auto a = MachineWithFile("machine_speaker.bin");
    if (a == nullptr) {
        CHECK(results, false);
        return;
    }
    tisple::RunOutcome r = RunToStop(*a);
    CHECK(results, r.reason == MachineStop::kReturned);
    CHECK(results, a->speaker.toggle_count == 1000);
    // the clicks are 469 cycles apart
    CHECK(results, tisple::SpeakerToggleCycle(*a, 1) - tisple::SpeakerToggleCycle(*a, 0) == 469);

    tisple::SpeakerRenderer speaker;
    std::vector<int16_t> samples;
    tisple::RenderSpeaker(speaker, *a, tisple::Cycles(*a), samples);
    CHECK(results, samples.size() == tisple::SamplesForCycles(tisple::Cycles(*a)));
    CHECK(results, samples.size() > 19000 && samples.size() < 21000); // about 0.46 s
    int crossings = 0;
    for (size_t i = 1; i < samples.size(); i++) {
        crossings += (samples[i - 1] < 0) != (samples[i] < 0) ? 1 : 0;
    }
    CHECK(results, crossings >= 990 && crossings <= 1010);

    std::string path = (std::filesystem::temp_directory_path() / "tisple_test.wav").string();
    std::string error;
    CHECK(results, tisple::WriteWav(path, samples, error));
    CHECK(results, std::filesystem::file_size(path) == 44 + samples.size() * 2);
    std::filesystem::remove(path);
}

// a snapshot taken mid-run, restored elsewhere, finishes with the same memory and screen
void TestSnapshot(TestResults& results) {
    auto a = MachineWithFile("machine_hires.bin");
    if (a == nullptr) {
        CHECK(results, false);
        return;
    }
    tisple::RunApple2(*a, 80000);
    std::vector<uint8_t> snapshot = tisple::SaveSnapshot(*a);
    RunToStop(*a);
    tisple::Framebuffer first;
    tisple::RenderFrame(a->video.done, tisple::VideoLook::kNtsc, first);

    std::unique_ptr<tisple::Apple2> b = tisple::NewApple2();
    std::string error;
    CHECK(results, tisple::LoadSnapshot(*b, snapshot.data(), snapshot.size(), error));
    CHECK(results, tisple::Cycles(*b) >= 80000 && tisple::Cycles(*b) < 80010);
    RunToStop(*b);
    tisple::Framebuffer second;
    tisple::RenderFrame(b->video.done, tisple::VideoLook::kNtsc, second);
    CHECK(results, tisple::Cycles(*a) == tisple::Cycles(*b));
    CHECK(results, std::equal(a->cpu.mem, a->cpu.mem + 0x10000, b->cpu.mem));
    CHECK(results, first.pixels == second.pixels);
    CHECK(results, b->stop_reason == MachineStop::kBrk);

    CHECK(results, !tisple::LoadSnapshot(*b, snapshot.data(), 10, error));
    snapshot[0] ^= 0xFF;
    CHECK(results, !tisple::LoadSnapshot(*b, snapshot.data(), snapshot.size(), error));
    CHECK(results, snapshot.size() < 400 * 1024);
}

} // namespace

void RunMachineRunTests(TestResults& results) {
    TestStops(results);
    TestRomBoot(results);
    TestSlotCard(results);
    TestScriptedKeys(results);
    TestSpeakerWav(results);
    TestSnapshot(results);
}

void RunMachineTests(TestResults& results) {
    RunMachineIoTests(results);
    RunMachineRunTests(results);
    RunMachineVideoTests(results);
}

} // namespace tisple_tests
