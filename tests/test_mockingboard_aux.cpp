// lib/auxmem's guard with lib/mockingboard's ticks running: 8 KB copies each way with
// interrupts on, through the card's own vector and Apple's two IIe roms, interrupts landing
// while RAMRD and RAMWRT are on; no tick lost, every byte where it belongs. Without the guard
// the same program goes wrong. And a song placed :in aux, copied in and played.

#include "machine/mockingboard.hpp"
#include "test_disk.hpp"
#include "test_mockingboard.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace tisple_tests {

namespace {

const char* kGuard = R"(
(import auxmem)
(import mockingboard mb)
(import mockingboard_song tune)
(tune.song hum (pattern a "l1 v12 c4 e4 g4 c5") (voice 0 a))
(macro counting (n)
    `(make (array u8 ,n) ,@(map (fn (i) (% i 251)) (range 0 n))))
(let doc (array u8 8192) (counting 8192) :in aux)
(let buf (array u8 8192))
(let ticks u16 0 :address 0x03C0) ; outside zero page, which the end puts back
(fn irq () void :interrupt
    (mb.service)
    (set ticks (+ ticks 1)))
(fn main () void
    (mb.init)
    (GUARD)
    (mb.play (addr_of (at hum 0)))
    (for r u8 0 3
        (auxmem.read (addr_of (at buf 0)) (addr_of (at doc 0)) 8192)
        (for i u16 0 8192
            (set (at buf i) (+ (at buf i) 1)))
        (auxmem.write (addr_of (at doc 0)) (addr_of (at buf 0)) 8192))
    (mb.off))
)";

constexpr uint16_t kGuardFix = 0x034C; // where the guard goes when RAMRD or RAMWRT is on

struct GuardWatch {
    const tisple::Apple2* a = nullptr;
    uint64_t in_read = 0;  // interrupts that came while RAMRD was on
    uint64_t in_write = 0; // and RAMWRT
};

void WatchGuard(void* ctx, const tisple::Machine& m) {
    GuardWatch& w = *static_cast<GuardWatch*>(ctx);
    if (tisple::GetRegs(m).pc != kGuardFix) {
        return;
    }
    w.in_read += w.a->sw.ramrd ? 1 : 0;
    w.in_write += w.a->sw.ramwrt ? 1 : 0;
}

std::string GuardProgram(bool guard) {
    std::string text = kGuard;
    size_t at = text.find("(GUARD)");
    text.replace(at, 7, guard ? "(auxmem.guard)" : "");
    return text;
}

// the machine with the program and the card in slot 4: through the card's ram vector (rom
// empty), or with Apple's rom and no card ram
std::unique_ptr<tisple::Apple2> GuardMachine(TestResults& results, const MbBuild& b,
                                             const char* rom_name) {
    if (rom_name[0] == 0) {
        std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
        a->lc.write_enable = true; // as a program's startup in the card leaves it
        return a;
    }
    std::vector<uint8_t> rom = OwnerRom(rom_name);
    if (rom.empty()) {
        std::printf("auxmem guard: %s not found, skipping\n", rom_name);
        return nullptr;
    }
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    std::string error;
    CHECK(results, tisple::LoadRom(*a, rom.data(), rom.size(), error));
    for (const tisple::MemorySegment& s : b.code.segments) {
        tisple::LoadSegment(*a, s, error);
    }
    tisple::LoadProgram(*a, b.code.image.data(), b.code.image.size(), b.code.origin, error);
    tisple::InsertMockingboard(*a, 4);
    return a;
}

struct GuardRun {
    bool returned = false;
    bool bytes = false; // every byte of both copies 3 more than it started
    bool ticks = false; // a tick every 17030 cycles from init to the end, each run
    uint64_t counted = 0;
    uint64_t taken = 0;
    uint64_t due = 0;
    GuardWatch watch;
};

GuardRun RunGuard(const MbBuild& b, tisple::Apple2& a) {
    GuardRun out;
    out.watch.a = &a;
    tisple::RunApple2Traced(a, 400 * kMbTick, WatchGuard, &out.watch);
    out.returned = a.stop_reason == tisple::MachineStop::kReturned;
    uint16_t doc = MbVarAddress(b, "t.doc");
    uint16_t buf = MbVarAddress(b, "t.buf");
    out.bytes = true;
    for (int i = 0; i < 8192; i++) {
        uint8_t want = static_cast<uint8_t>(i % 251 + 3);
        out.bytes = out.bytes && a.aux[doc + i] == want && a.cpu.mem[buf + i] == want;
    }
    out.taken = a.cpu.irqs_taken;
    out.counted = a.cpu.mem[0x03C0] | (a.cpu.mem[0x03C1] << 8);
    out.due = a.cpu.cycles / kMbTick;
    out.ticks = out.counted == out.taken && out.taken + 2 >= out.due && out.taken <= out.due;
    return out;
}

void GuardTests(TestResults& results) {
    MbBuild b;
    MbBuildText(results, "t.tsp", GuardProgram(true), b);
    if (!b.ok) {
        return;
    }
    for (const char* rom : {"", "Apple2e.rom", "Apple2e_Enhanced.rom"}) {
        std::unique_ptr<tisple::Apple2> a = GuardMachine(results, b, rom);
        if (a == nullptr) {
            continue;
        }
        GuardRun run = RunGuard(b, *a);
        CHECK(results, run.returned && run.bytes && run.ticks);
        if (!run.ticks) {
            std::fprintf(stderr, "auxmem guard: %llu ticks counted, %llu taken, %llu due\n",
                         static_cast<unsigned long long>(run.counted),
                         static_cast<unsigned long long>(run.taken),
                         static_cast<unsigned long long>(run.due));
        }
        // the enhanced rom switches to main memory itself, so the guard's own way is not taken
        bool enhanced = std::string(rom) == "Apple2e_Enhanced.rom";
        CHECK(results, enhanced || (run.watch.in_read > 3 && run.watch.in_write > 3));
        std::printf("auxmem guard (%s): %llu ticks landed in a read, %llu in a write\n",
                    rom[0] == 0 ? "card ram vector" : rom,
                    static_cast<unsigned long long>(run.watch.in_read),
                    static_cast<unsigned long long>(run.watch.in_write));
    }
    // the same with no guard, through the card's vector: the handler runs from aux memory
    MbBuild bare;
    MbBuildText(results, "t.tsp", GuardProgram(false), bare);
    if (!bare.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = GuardMachine(results, bare, "");
    GuardRun run = RunGuard(bare, *a);
    CHECK(results, !(run.returned && run.bytes && run.ticks));
}

// a song in aux memory, copied in when it starts
const char* kAuxSong = R"(
(import auxmem)
(import mockingboard mb)
(import mockingboard_song tune)
(tune.song hum (pattern a "l4 v12 c4 e4 g4") (voice 0 a) :in aux)
(let playing (array u8 64))
(fn irq () void :interrupt (mb.service))
(fn main () void
    (mb.init)
    (auxmem.guard)
    (auxmem.read (addr_of (at playing 0)) (addr_of (at hum 0)) 64)
    (mb.play (addr_of (at playing 0)))
    (for i u8 0 40
        (wait_cycles 17030))
    (mb.off))
)";

void AuxSongTests(TestResults& results) {
    MbBuild b;
    MbBuildText(results, "t.tsp", kAuxSong, b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    a->lc.write_enable = true;
    tisple::RunApple2(*a, 60 * kMbTick);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    // the song's header in aux memory, not in main
    uint16_t hum = MbVarAddress(b, "t.hum");
    CHECK(results, a->aux[hum] == 12 && a->aux[hum + 12] == 18);
    // its notes on voice 0, from the first tick
    const tisple::MockingboardState& mb = a->slots.mockingboard;
    uint64_t first = 0;
    for (uint64_t n = 0; n < mb.log_count && first == 0; n++) {
        const tisple::PsgWrite& w = tisple::MockingboardWrite(*a, n);
        if (w.chip == 0 && w.reg == 0 && w.value != 0) {
            first = w.cycle - 2000;
        }
    }
    Voices v = ReadVoices(*a, first, 36);
    const uint16_t notes[3] = {Period(Note('c', false, 4)), Period(Note('e', false, 4)),
                               Period(Note('g', false, 4))};
    bool ok = true;
    for (int t = 0; t < 36; t++) {
        ok = ok && v.period[0][t] == notes[(t / 4) % 3] && v.volume[0][t] == 12;
    }
    CHECK(results, ok);
}

// the demo's ticks through the guard, which the card's vector now reaches first
void GuardCostTests(TestResults& results) {
    std::ifstream file("examples/mockingboard_demo.tsp");
    std::stringstream text;
    text << file.rdbuf();
    std::string program = "(import auxmem)\n" + text.str();
    size_t init = program.find("    (mb.init)\n");
    CHECK(results, init != std::string::npos);
    if (init == std::string::npos) {
        return;
    }
    program.insert(init + 14, "    (auxmem.guard)\n");
    MbBuild b;
    MbBuildText(results, "examples/guarded_demo.tsp", program, b);
    if (!b.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    a->lc.write_enable = true;
    tisple::ScheduleKey(*a, 300 * tisple::kCyclesPerFrame, '1');
    tisple::ScheduleKey(*a, 600 * tisple::kCyclesPerFrame, 0x1B);
    TickCost cost;
    cost.entry = 0x0340; // the guard's way in from the card's vector
    tisple::RunApple2Traced(*a, 700 * tisple::kCyclesPerFrame, MeasureTick, &cost);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, cost.count > 590 && cost.count < 605);
    if (cost.count == 0) {
        return;
    }
    std::printf("auxmem guard: the demo's tick through it %llu cycles on average\n",
                static_cast<unsigned long long>(cost.total / cost.count));
}

} // namespace

void RunMockingboardAuxTests(TestResults& results) {
    GuardTests(results);
    GuardCostTests(results);
    AuxSongTests(results);
}

} // namespace tisple_tests
