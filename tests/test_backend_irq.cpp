// Interrupt handlers: IRQs raised at chosen cycles on the 6502 against the interpreter's model
// (the handler run between two steps), through the vector at $FFFE straight to the entry and
// through a ROM-like routine to $3FE; the handler clobbering nothing of the code it stops;
// the mid-level passes leaving what the handler shares alone; the machine with Apple's roms;
// examples/ticks.tsp on the emulated Mockingboard; the checker's rules

#include "machine/mockingboard.hpp"
#include "test_backend.hpp"
#include "test_check.hpp"
#include "test_disk.hpp"

#include <cstdio>
#include <memory>
#include <string>

namespace tisple_tests {

namespace {

// what a II+ or unenhanced IIe does with an IRQ, in two instructions of our own: A into $45,
// then through $3FE
const std::uint8_t kRomLike[] = {0x85, 0x45, 0x6C, 0xFE, 0x03};
constexpr std::uint16_t kRomLikeAt = 0xFF00;

enum class Route : std::uint8_t {
    Direct, // $FFFE is the entry's STA $45, as with the card's ram in
    Rom,    // $FFFE goes to a rom-like routine, which jumps through $3FE
};

struct IrqRun {
    Diff d;
    std::uint64_t sim_irqs = 0;
    std::uint64_t interp_irqs = 0;
    bool ok = false;
};

// t.main on both: IRQs at these cycles (from main's start) on the 6502, the handler before
// these steps in the interpreter; same globals, same number of interrupts taken
void RunIrqs(IrqRun& run, const Compiled& c, Route route, const std::vector<std::uint64_t>& cycles,
             const std::vector<std::uint64_t>& steps) {
    Diff& d = run.d;
    if (!StartDiff(d, c)) {
        return;
    }
    tisple::Machine& m = *d.sim.machine;
    std::uint16_t vector = c.code.irq_direct;
    if (route == Route::Rom) {
        for (std::size_t i = 0; i < sizeof(kRomLike); i++) {
            m.mem[kRomLikeAt + i] = kRomLike[i];
        }
        vector = kRomLikeAt;
    }
    m.mem[0xFFFE] = static_cast<std::uint8_t>(vector & 0xFF);
    m.mem[0xFFFF] = static_cast<std::uint8_t>(vector >> 8);
    // the call's own JSR from the harness takes a few cycles first: count from now
    for (std::uint64_t k : cycles) {
        tisple::RequestIrq(m, m.cycles + k);
    }
    d.in.irq_steps = steps;
    run.ok = DiffCall(d, "t.main", {});
    run.sim_irqs = m.irqs_taken;
    run.interp_irqs = d.in.irqs_taken;
    if (run.ok && run.sim_irqs != run.interp_irqs) {
        d.message = std::to_string(run.sim_irqs) + " interrupts on the 6502, " +
                    std::to_string(run.interp_irqs) + " in the interpreter";
        run.ok = false;
    }
}

void CheckIrqs(TestResults& results, const std::string& text, Route route,
               const std::vector<std::uint64_t>& cycles, const std::vector<std::uint64_t>& steps,
               std::uint64_t want) {
    Compiled c = CompileMainText(text);
    IrqRun run;
    RunIrqs(run, c, route, cycles, steps);
    bool ok = run.ok && run.sim_irqs == want;
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s (%llu interrupts)\n", text.c_str(),
                     run.d.message.c_str(), static_cast<unsigned long long>(run.sim_irqs));
    }
    CHECK(results, ok);
}

// main multiplies and divides through the runtime, indexes with X and Y and keeps a pointer
// and a frame; the handler does all of that too, in a function of its own
const char* const kBusy = R"(
(let ticks u16 0)
(let hacc u16 7)
(let htab (array u8 16))
(let sums (array u16 8))
(let out u16 0)
(let cells (array u8 64))

(fn churn ((k u8)) u16
    (let s u16 (* (as u16 k) 2731))
    (for j u8 0 (& k 7)
        (set (at htab (& (+ j k) 15)) (+ (at htab j) (as u8 (/ s 13)))))
    (% s 1009))

(fn tick () void :interrupt
    (set ticks (+ ticks 1))
    (set hacc (+ (* hacc 3) (churn (as u8 ticks)))))

(fn mix ((p (ptr u8)) (n u8)) u16
    (let t u16 0)
    (for i u8 0 n
        (store (+ p (as u16 i)) (+ (load (+ p (as u16 i))) (* i 7)))
        (set t (+ t (/ (* (as u16 (load (+ p (as u16 i)))) 77) (+ (as u16 i) 3)))))
    t)

(fn main () void
    (for r u8 0 8
        (set (at sums r) (mix (addr_of (at cells (* r 8))) 8))
        (set out (+ out (* (at sums r) (as u16 (+ r 1)))))))
)";

void HandlerTests(TestResults& results) {
    // one interrupt, at many places in main, both ways in
    Compiled c = CompileMainText(kBusy);
    CHECK(results, c.ok);
    if (!c.ok) {
        std::fprintf(stderr, "%s\n", c.errors.c_str());
        return;
    }
    int bad = 0;
    for (int k = 0; k < 120 && bad < 3; k++) {
        IrqRun run;
        Route route = k % 2 == 0 ? Route::Direct : Route::Rom;
        RunIrqs(run, c, route, {static_cast<std::uint64_t>(40 + k * 97)}, {50});
        if (!run.ok || run.sim_irqs != 1) {
            bad++;
            std::fprintf(stderr, "interrupt at cycle %d: %s\n", 40 + k * 97, run.d.message.c_str());
        }
    }
    CHECK(results, bad == 0);
    // many, some while the handler runs (they wait for its RTI)
    std::vector<std::uint64_t> cycles;
    std::vector<std::uint64_t> steps;
    for (int k = 0; k < 40; k++) {
        cycles.push_back(static_cast<std::uint64_t>(30 + k * 211 + (k % 3) * 5));
        steps.push_back(static_cast<std::uint64_t>(20 + k * 15));
    }
    CheckIrqs(results, kBusy, Route::Direct, cycles, steps, 40);
    CheckIrqs(results, kBusy, Route::Rom, cycles, steps, 40);
}

// fuzz seed 5064309 (irq): u8 divisions by a variable call rt.divu8.ge, a second entry of
// the routine, which the entry did not count as the runtime: rt_r and rt_s went unsaved
const char* const kDivide = R"(
(let hq u8 200)
(let hd u8 7)
(let hs u16 0)
(let out (array u8 64))
(fn tick () void :interrupt
    (set hs (+ hs 1))
    (set hq (+ (/ hq (| hd 1)) (/ (+ hq 50) (| hd 3))))
    (set hd (+ hd 1)))
(fn main () void
    (for i u8 0 64
        (let d u8 (| (* i 3) 1))
        (set (at out i) (+ (/ (+ i 200) d) (% (* i 7) d)))))
)";

void DivideTests(TestResults& results) {
    Compiled c = CompileMainText(kDivide);
    CHECK(results, c.ok);
    int bad = 0;
    for (int k = 0; k < 200 && bad < 3 && c.ok; k++) {
        IrqRun run;
        RunIrqs(run, c, k % 2 == 0 ? Route::Direct : Route::Rom,
                {static_cast<std::uint64_t>(30 + k * 23)}, {40});
        if (!run.ok || run.sim_irqs != 1) {
            bad++;
            std::fprintf(stderr, "divisions, interrupt at cycle %d: %s\n", 30 + k * 23,
                         run.d.message.c_str());
        }
    }
    CHECK(results, bad == 0);
}

void MaskTests(TestResults& results) {
    // a 16-bit counter read whole with interrupts off; the interrupt waits for (cli)
    CheckIrqs(results, R"(
(let ticks u16 255)
(let seen u16 0)
(let work u16 0)
(fn tick () void :interrupt (set ticks (+ ticks 1)))
(fn main () void
    (sei)
    (for i u8 0 100 (set work (+ work (* (as u16 i) 3))))
    (set seen ticks)
    (cli)
    (for i u8 0 50 (set work (+ work 1))))
)",
              Route::Direct, {20}, {5}, 1);
    // waiting for the handler: a loop the passes may not empty or hoist
    CheckIrqs(results, R"(
(let flag u8 0)
(let ticks u8 0)
(let out u16 0)
(fn tick () void :interrupt
    (set ticks (+ ticks 1))
    (if (= ticks 3) (set flag 1)))
(fn main () void
    (while (= flag 0)
        (set out (+ out 1)))
    (set out 0))
)",
              Route::Rom, {200, 900, 1500}, {30, 60, 90}, 3);
}

// the passes keep every read and write of what the handler shares where the program has it
void PassTests(TestResults& results) {
    Compiled c = CompileMainText(R"(
(let mode u8 0)
(let ticks u8 0)
(let seen u8 0)
(let other u8 0)
(fn tick () void :interrupt
    (if (= mode 1) (set ticks (+ ticks 1))))
(fn main () void
    (set mode 1)
    (set other 1)
    (for i u8 0 10 (set seen (+ seen ticks)))
    (set mode 2)
    (set other 2)
    (set mode 0)
    (set other 0))
)");
    CHECK(results, c.ok);
    if (!c.ok) {
        return;
    }
    // three stores of mode, one of other (the others are dead)
    const tisple::Program& program = c.built.program;
    std::uint16_t mode =
        c.code.layout.vars[static_cast<std::size_t>(tisple::FindVar(program, "t.mode"))].address;
    std::uint16_t other =
        c.code.layout.vars[static_cast<std::size_t>(tisple::FindVar(program, "t.other"))].address;
    std::uint16_t ticks =
        c.code.layout.vars[static_cast<std::size_t>(tisple::FindVar(program, "t.ticks"))].address;
    int mode_stores = 0;
    int other_stores = 0;
    int ticks_reads = 0;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.name != "t.main") {
            continue;
        }
        for (std::size_t i = span.first; i < span.end; i++) {
            const tisple::AsmItem& item = c.code.as.items[i];
            if (item.kind != tisple::ItemKind::Ins || item.value.sym < 0) {
                continue;
            }
            std::uint32_t at = c.code.as.symbols[static_cast<std::size_t>(item.value.sym)].value +
                               static_cast<std::uint32_t>(item.value.offset);
            bool store = item.mn == tisple::Mn::STA || item.mn == tisple::Mn::STX ||
                         item.mn == tisple::Mn::STY;
            mode_stores += store && at == mode ? 1 : 0;
            other_stores += store && at == other ? 1 : 0;
            ticks_reads += !store && at == ticks ? 1 : 0;
        }
    }
    CHECK(results, mode_stores == 3);
    CHECK(results, other_stores == 1);
    CHECK(results, ticks_reads >= 1);
}

// Apple's roms route the IRQ themselves (II+, unenhanced IIe): $FA40 saves A in $45 and jumps
// through $3FE. The program installs its entry there and takes it out again
void RomTests(TestResults& results) {
    for (const char* name : {"Apple2_Plus.rom", "Apple2e.rom", "Apple2e_Enhanced.rom"}) {
        std::vector<std::uint8_t> rom = OwnerRom(name);
        if (rom.empty()) {
            std::printf("interrupts: %s not found, skipping\n", name);
            continue;
        }
        Compiled c = CompileMainText(kBusy);
        std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
        std::string error;
        CHECK(results, tisple::LoadRom(*a, rom.data(), rom.size(), error));
        CHECK(results, tisple::LoadProgram(*a, c.code.image.data(), c.code.image.size(),
                                           c.code.origin, error));
        // DOS's vector, which the program must put back
        a->cpu.mem[0x3FE] = 0x65;
        a->cpu.mem[0x3FF] = 0xFF;
        for (int k = 0; k < 20; k++) {
            tisple::RequestIrq(a->cpu, a->cpu.cycles + 2000 + static_cast<std::uint64_t>(k) * 2500);
        }
        tisple::RunApple2(*a, 5 * 1000 * 1000);
        CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
        if (a->cpu.irqs_taken != 20) {
            std::fprintf(stderr, "%s: %llu interrupts in %llu cycles\n", name,
                         static_cast<unsigned long long>(a->cpu.irqs_taken),
                         static_cast<unsigned long long>(a->cpu.cycles));
        }
        CHECK(results, a->cpu.irqs_taken == 20);
        // the interpreter's main with the handler run 20 times
        Diff d;
        d.in.irq_steps.assign(20, 10);
        bool ran = StartDiff(d, c) && tisple::CallFunction(d.in, c.built.program.main, {}).kind ==
                                          tisple::StopKind::Finished;
        CHECK(results, ran && d.in.irqs_taken == 20);
        const tisple::Program& program = c.built.program;
        // the end puts zero page back as it was: the globals outside it (htab the handler's,
        // sums and cells main's)
        bool same = true;
        for (std::size_t v = 0; v < program.vars.size(); v++) {
            const tisple::VarPlace& place = c.code.layout.vars[v];
            bool outside = program.vars[v].function < 0 && place.address >= 0x100;
            for (std::uint32_t i = 0; outside && i < place.size; i++) {
                std::uint16_t at = static_cast<std::uint16_t>(place.address + i);
                if (d.in.memory[at] != a->cpu.mem[at] && same) {
                    std::fprintf(stderr, "%s: %s byte %u: interpreter %d, machine %d\n", name,
                                 program.vars[v].name.c_str(), i, d.in.memory[at], a->cpu.mem[at]);
                    same = false;
                }
            }
        }
        CHECK(results, same);
        CHECK(results, a->cpu.mem[0x3FE] == 0x65 && a->cpu.mem[0x3FF] == 0xFF);
        if (same && a->cpu.irqs_taken == 20) {
            std::printf("interrupts: 20 through %s, globals as the interpreter's\n", name);
        }
    }
}

// a program in the language card's ram: the cpu reads $FFFE there, which the startup points at
// the entry (and $FFFA and $FFFC at the roms' NMI and RESET), with no code or data over them
void CardTests(TestResults& results) {
    const char* text = R"(
(let big (array u8 12000))
(let more (array u8 12000))
(let most (array u8 12000))
(let hits (array u8 4))
(fn tick () void :interrupt (set (at hits 0) (+ (at hits 0) 1)))
(fn main () void
    (for i u16 0 12000
        (set (at big i) (as u8 i))
        (set (at more i) (as u8 (>> i 3)))
        (set (at most i) (as u8 (>> i 5)))))
)";
    Compiled c = CompileMainText(text);
    CHECK(results, c.ok && c.code.card_ram);
    if (!c.ok || !c.code.card_ram) {
        std::fprintf(stderr, "%s\n", c.errors.c_str());
        return;
    }
    for (const tisple::MemorySegment& s : c.code.segments) {
        CHECK(results, s.address + s.bytes.size() <= 0xFFFA);
    }
    for (const char* name : {"", "Apple2e.rom"}) {
        std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
        std::string error;
        std::vector<std::uint8_t> rom = OwnerRom(name);
        if (*name != 0 && rom.empty()) {
            continue;
        }
        if (!rom.empty()) {
            CHECK(results, tisple::LoadRom(*a, rom.data(), rom.size(), error));
        }
        for (const tisple::MemorySegment& s : c.code.segments) {
            CHECK(results, tisple::LoadSegment(*a, s, error));
        }
        CHECK(results, tisple::LoadProgram(*a, c.code.image.data(), c.code.image.size(),
                                           c.code.origin, error));
        for (int k = 0; k < 20; k++) {
            tisple::RequestIrq(a->cpu, a->cpu.cycles + 5000 + static_cast<std::uint64_t>(k) * 9000);
        }
        tisple::RunApple2(*a, 20 * 1000 * 1000);
        CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
        std::uint16_t hits =
            c.code.layout.vars[static_cast<std::size_t>(tisple::FindVar(c.built.program, "t.hits"))]
                .address;
        bool counted = a->cpu.irqs_taken == 20 && PeekMemory(*a, hits) == 20;
        if (!counted) {
            std::fprintf(stderr, "card (%s): %llu interrupts, %d counted\n",
                         *name ? name : "no rom",
                         static_cast<unsigned long long>(a->cpu.irqs_taken), PeekMemory(*a, hits));
        }
        CHECK(results, counted);
        const std::uint8_t* top = a->lc.e000 + 0x1FFA;
        CHECK(results, top[0] == 0xFB && top[1] == 0x03 && top[2] == 0x62 && top[3] == 0xFA &&
                           (top[4] | (top[5] << 8)) == c.code.irq_direct);
    }
}

// examples/ticks.tsp with a Mockingboard's timer: an interrupt every 17047 cycles (its latch
// and 2) until the program has counted ten seconds and returned. The simulator has no card, so
// there the interrupts are asked for at those cycles; the machine has the emulated card in slot
// 4, through the card's ram vector and through Apple's IIe rom
void ExampleTests(TestResults& results) {
    Compiled c = CompileFile("examples/ticks.tsp");
    CHECK(results, c.ok);
    if (!c.ok) {
        std::fprintf(stderr, "%s\n", c.errors.c_str());
        return;
    }
    // the simulator, $FFFE straight to the entry
    tisple::SimRunner sim;
    std::string error;
    CHECK(results, tisple::StartCompiled(sim, c.code, error));
    tisple::Machine& m = *sim.machine;
    m.mem[0xFFFE] = static_cast<std::uint8_t>(c.code.irq_direct & 0xFF);
    m.mem[0xFFFF] = static_cast<std::uint8_t>(c.code.irq_direct >> 8);
    for (int k = 1; k <= 700; k++) {
        tisple::RequestIrq(m, m.cycles + static_cast<std::uint64_t>(k) * 17047);
    }
    tisple::SimCall call =
        tisple::CallCompiled(sim, c.built.program, c.code, c.built.program.main, {}, 20000000);
    CHECK(results, call.returned && m.irqs_taken >= 600 && m.irqs_taken < 610);
    // "10" in the top left corner, normal text
    CHECK(results, m.mem[0x400] == 0xB1 && m.mem[0x401] == 0xB0);
    // the machine with the card: no rom, the vector in the card's ram; then Apple's IIe rom
    for (const char* name : {"", "Apple2e.rom"}) {
        std::vector<std::uint8_t> rom = OwnerRom(name);
        if (*name != 0 && rom.empty()) {
            continue;
        }
        std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
        if (!rom.empty()) {
            CHECK(results, tisple::LoadRom(*a, rom.data(), rom.size(), error));
        }
        CHECK(results, tisple::LoadProgram(*a, c.code.image.data(), c.code.image.size(),
                                           c.code.origin, error));
        if (rom.empty()) {
            a->lc.read_ram = true;
            a->lc.e000[0x1FFE] = static_cast<std::uint8_t>(c.code.irq_direct & 0xFF);
            a->lc.e000[0x1FFF] = static_cast<std::uint8_t>(c.code.irq_direct >> 8);
        }
        tisple::InsertMockingboard(*a, 4);
        tisple::RunOutcome out = tisple::RunApple2(*a, 20000000);
        CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
        // 600 ticks of 17047 cycles, and the time to clear the screen and show the seconds
        bool counted = a->cpu.irqs_taken == 600 && out.cycles > 600 * 17047 &&
                       out.cycles < 600 * 17047 + 40000;
        if (!counted) {
            std::fprintf(stderr, "ticks (%s): %llu interrupts in %llu cycles\n",
                         *name ? name : "no rom",
                         static_cast<unsigned long long>(a->cpu.irqs_taken),
                         static_cast<unsigned long long>(out.cycles));
        }
        CHECK(results, counted);
        CHECK(results, a->cpu.mem[0x400] == 0xB1 && a->cpu.mem[0x401] == 0xB0);
        // the program turned the timer's interrupt off at the end
        CHECK(results, a->cpu.irq_level_at == UINT64_MAX);
    }
}

void CheckerTests(TestResults& results) {
    ExpectError(results, "(fn t ((x u8)) void :interrupt)\n(fn main () void)",
                "t.tsp:1:1: an interrupt handler takes nothing and gives nothing; write (fn t () "
                "void :interrupt ...), not (fn t ((x u8)) void :interrupt ...)");
    ExpectError(results, "(fn a () void :interrupt)\n(fn b () void :interrupt)\n(fn main () void)",
                "t.tsp:2:15: a program has one interrupt handler, and t.a is it; poll every "
                "device there");
    ExpectError(results, "(fn a () void :fast)\n(fn main () void)",
                "t.tsp:1:15: :fast is no fn option; the one there is is :interrupt");
    ExpectError(results, "(fn a () void :interrupt)\n(fn main () void (a))",
                "t.tsp:2:18: a is the interrupt handler: an IRQ runs it, a call never does");
    ExpectError(results, "(fn a () void :interrupt)\n(fn main () void (let f (fn () void) a))",
                "t.tsp:2:38: a is the interrupt handler: an IRQ runs it, no pointer does");
    ExpectError(results,
                "(let g u8 0)\n(fn h () void (set g 1))\n(fn a () void :interrupt (h))\n"
                "(fn main () void (h))",
                "t.tsp:2:1: t.h runs in the interrupt handler t.a and in main's code, which an "
                "interrupt may stop in the middle of t.h (one static frame); give the handler a "
                "copy of its own");
    ExpectError(results, "(fn a () void :interrupt (cli))\n(fn main () void)",
                "t.tsp:1:26: (cli) where the interrupt handler runs would let interrupts nest; "
                "leave it out (the handler's return turns them on again)");
    ExpectError(results, "(fn a () void :interrupt)",
                "t.tsp:1:1: t.a is an interrupt handler, which the startup installs before main; "
                "write a main");
    ExpectError(results, "(fn main () void (sei 1))", "t.tsp:1:18: sei takes nothing; write (sei)");
}

} // namespace

void RunBackendIrqTests(TestResults& results) {
    HandlerTests(results);
    DivideTests(results);
    MaskTests(results);
    PassTests(results);
    RomTests(results);
    CardTests(results);
    ExampleTests(results);
    CheckerTests(results);
}

} // namespace tisple_tests
