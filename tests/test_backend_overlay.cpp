// overlay groups (backend/overlay.hpp, overlay_plan.hpp): a program whose main loop dispatches
// on a screen and does not fit gets each screen's code in a group, loaded from aux memory into
// one region; the same with the screens marked :overlay where the dispatch is not the loop's own;
// groups taking turns call after call; what never goes in a group; interrupts that land during
// a load and in group code on the emulated IIe; and every one on the simulator and the
// interpreter alike

#include "backend/overlay.hpp"
#include "machine/apple2.hpp"
#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

bool Report(const Compiled& c) {
    if (!c.ok) {
        std::fprintf(stderr, "%s\n", c.errors.c_str());
    }
    return c.ok;
}

// the overlay group a function's code is in, or -1
std::int32_t GroupOf(const Compiled& c, const char* name) {
    std::int32_t f = tisple::FindFunction(c.built.program, name);
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.function == f && f >= 0) {
            return span.overlay_group;
        }
    }
    return -1;
}

bool Noted(const Compiled& c, const char* part) {
    for (const tisple::Diagnostic& w : c.code.warnings) {
        if (w.message.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// loads every segment and the file, then interrupts every `every` cycles from 3000 on
// (`irqs` of them), and runs to the final RTS
bool RunOnMachine(const Compiled& c, tisple::Apple2& a, std::uint64_t cycles, int irqs = 0,
                  std::uint64_t every = 0, tisple::MachineTraceFn trace = nullptr,
                  void* ctx = nullptr) {
    std::string error;
    for (const tisple::MemorySegment& s : c.code.segments) {
        if (!tisple::LoadSegment(a, s, error)) {
            return false;
        }
    }
    if (!tisple::LoadProgram(a, c.code.image.data(), c.code.image.size(), c.code.origin, error)) {
        return false;
    }
    for (int k = 0; k < irqs; k++) {
        tisple::RequestIrq(a.cpu, a.cpu.cycles + 3000 + static_cast<std::uint64_t>(k) * every);
    }
    if (trace != nullptr) {
        tisple::RunApple2Traced(a, cycles, trace, ctx);
    } else {
        tisple::RunApple2(a, cycles);
    }
    return a.stop_reason == tisple::MachineStop::kReturned;
}

// three screens of stores, each moving to the next now and then; filler takes main memory and
// the card. With `step`, the dispatch is in a function the loop calls (not the loop's own), and
// the screens are marked :overlay when `marked`
std::string Screens(int filler, bool step, bool marked) {
    std::string mark = marked ? " :overlay" : "";
    std::string dispatch = "(match screen (0 (title i)) (1 (fight i)) (_ (reward i)))";
    std::string text = "(let big (array u8 " + std::to_string(filler) + "))\n" + R"(
(let a (array u8 13000))
(macro counting (n)
    `(make (array u8 ,n) ,@(map (fn (i) (% i 251)) (range 0 n))))
(let c (array u8 9000) (counting 9000))
(const TAG (array u8 4) (make (array u8 4) 1 2 3 4) :in aux)
(let buf (array u8 1000))
(let screen u8 0)
(let visits (array u8 3))
(let result u16 0 :address 0x0300)
(macro stores (n k)
    (let out (list))
    (for i 0 n (set out (append out (list `(set (at buf ,(+ i k)) (+ (at c ,(* i 13)) ,i))))))
    `(do ,@out))
)";
    text += "(fn title ((i u8)) void" + mark +
            "\n    (stores 100 0) (set (at visits 0) (+ (at visits 0) 1))\n"
            "    (if (= (& i 3) 3) (set screen 1)))\n";
    text += "(fn fight ((i u8)) void" + mark +
            "\n    (stores 100 200) (set (at visits 1) (+ (at visits 1) 1))\n"
            "    (if (= (& i 3) 1) (set screen 2)))\n";
    text += "(fn reward ((i u8)) void" + mark +
            "\n    (stores 100 400) (set (at visits 2) (+ (at visits 2) i))\n"
            "    (if (= (& i 3) 0) (set screen 0)))\n";
    if (step) {
        text += "(fn step ((i u8)) void " + dispatch + ")\n";
        dispatch = "(step i)";
    }
    text += "(fn main () void\n    (for i u8 0 40 " + dispatch + ")\n" + R"(
    (set (at a 12999) (at buf 450))
    (set result (+ (+ (as u16 (at buf 7)) (as u16 (at buf 207)))
                   (+ (as u16 (at buf 407)) (as u16 (at big 3))))))
)";
    return text;
}

// the smallest filler (in steps of 250) with which the program needs its groups, or 0
int NeedsGroups(bool step, bool marked) {
    tisple::BuildOptions options;
    options.use_bank1 = false;
    for (int filler = 4000; filler <= 16000; filler += 250) {
        Compiled c = CompileMainWith(Screens(filler, step, marked), options);
        if (!c.ok) {
            return 0;
        }
        if (!c.code.overlays.groups.empty()) {
            return filler;
        }
    }
    return 0;
}

void CheckScreens(TestResults& results, const char* what, bool step, bool marked) {
    int filler = NeedsGroups(step, marked);
    CHECK(results, filler > 0);
    if (filler == 0) {
        std::fprintf(stderr, "overlays (%s): no filler made a program that needs groups\n", what);
        return;
    }
    tisple::BuildOptions options;
    options.use_bank1 = false;
    Compiled c = CompileMainWith(Screens(filler, step, marked), options);
    if (!Report(c)) {
        CHECK(results, false);
        return;
    }
    CheckSegmentsApart(results, c);
    const tisple::OverlayBuilt& built = c.code.overlays;
    CHECK(results, built.groups.size() == 3);
    std::int32_t title = GroupOf(c, "t.title");
    std::int32_t fight = GroupOf(c, "t.fight");
    std::int32_t reward = GroupOf(c, "t.reward");
    CHECK(results, title >= 0 && fight >= 0 && reward >= 0 && title != fight && fight != reward &&
                       title != reward);
    CHECK(results, GroupOf(c, "t.main") < 0 && GroupOf(c, "t.step") < 0);
    // the region holds the biggest; each image in aux memory
    std::uint32_t biggest = 0;
    for (const tisple::OverlayImage& image : built.groups) {
        biggest = std::max(biggest, image.bytes);
        CHECK(results, image.aux >= 0x0800 && image.aux + image.bytes <= 0xC000);
    }
    CHECK(results, built.region_size == biggest);
    CHECK(results, Noted(c, "overlay region"));
    // a root is called through its stub, JMP to its load until its group is in
    std::int32_t f = tisple::FindFunction(c.built.program, "t.fight");
    std::uint16_t stub = c.code.function_address[static_cast<std::size_t>(f)];
    CHECK(results, stub < built.region || stub >= built.region + built.region_size);
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.main", {});
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    CHECK(results, RunOnMachine(c, *a, 20000000));
    CHECK(results,
          a->cpu.mem[0x0300] == d.in.memory[0x0300] && a->cpu.mem[0x0301] == d.in.memory[0x0301]);
    std::printf("overlays (%s): 3 groups of %u, %u and %u bytes in a %u-byte region, filler %d\n",
                what, built.groups[0].bytes, built.groups[1].bytes, built.groups[2].bytes,
                built.region_size, filler);
}

// with no mark, a loop's own dispatch makes the groups; a dispatch in a function the loop
// calls makes none (its arms may change every pass), but marks do
void TestScreens(TestResults& results) {
    CheckScreens(results, "the loop's dispatch", false, false);
    CheckScreens(results, "marked", true, true);
    CHECK(results, NeedsGroups(true, false) == 0);
}

// groups forced (BuildOptions::overlay_test) where they call each other's roots through
// resident code and take turns every call: each call loads, and the result is the same
void TestTakingTurns(TestResults& results) {
    const char* text = R"(
(let out (array u8 16))
(let total u16 0 :address 0x0300)
(let seed u8 0)
(fn add ((x u8) (y u8)) u8 (+ (* x 3) y))
(fn red ((x u8)) u8 (set (at out 0) (add x 1)) (+ (at out 0) 7))
(fn green ((x u8)) u8 (set (at out 1) (add x 2)) (^ (at out 1) 0x55))
(fn blue ((x u8)) u16 (set (at out 2) (add x 3)) (+ (as u16 (at out 2)) 300))
(fn main () void
    (for i u8 0 30
        (set total (+ total (as u16 (red i))))
        (set seed (+ seed 37))
        (set total (+ total (as u16 (green seed))))
        (set total (+ total (blue i)))))
)";
    tisple::BuildOptions options;
    options.overlay_test = 1;
    options.no_inline = {"t.red", "t.green", "t.blue", "t.add"};
    Compiled c = CompileMainWith(text, options);
    if (!Report(c)) {
        CHECK(results, false);
        return;
    }
    CHECK(results, c.code.overlays.groups.size() >= 2);
    CHECK(results, GroupOf(c, "t.add") < 0); // all three call it
    std::string listing = tisple::ListProgram(c.built.program, c.code);
    CHECK(results, listing.find("ov.copy") != std::string::npos);
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.main", {});
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    CHECK(results, RunOnMachine(c, *a, 20000000));
    CHECK(results,
          a->cpu.mem[0x0300] == d.in.memory[0x0300] && a->cpu.mem[0x0301] == d.in.memory[0x0301]);
}

// what never goes in a group: the interrupt handler and what it calls, a function pointer's
// target, main; a mark on them says it stays resident
void TestKeptOut(TestResults& results) {
    const char* text = R"(
(let hits u8 0)
(let out (array u8 8))
(const TAG (array u8 2) (make (array u8 2) 1 2) :in aux)
(fn bump () void :overlay (set hits (+ hits 1)))
(fn tick () void :interrupt (bump))
(fn twice ((x u8)) u8 :overlay (+ x x))
(fn plain ((x u8)) u8 (+ x 9))
(fn main () void
    (let p (fn (u8) u8) twice)
    (set (at out 0) (p 21))
    (set (at out 1) (plain (io_read u8 0xC0F0))))
)";
    tisple::BuildOptions options;
    options.overlay_test = 1;
    options.no_inline = {"t.plain"};
    Compiled c = CompileMainWith(text, options);
    if (!Report(c)) {
        CHECK(results, false);
        return;
    }
    CHECK(results, GroupOf(c, "t.tick") < 0 && GroupOf(c, "t.bump") < 0);
    CHECK(results, GroupOf(c, "t.twice") < 0 && GroupOf(c, "t.main") < 0);
    CHECK(results, GroupOf(c, "t.plain") >= 0);
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.main", {});
}

// where each interrupt landed: the instruction before the handler's entry
struct Landings {
    std::uint16_t entry = 0;
    std::uint16_t prev = 0;
    std::uint16_t copy_start = 0;
    std::uint16_t copy_end = 0;
    std::uint16_t region = 0;
    std::uint32_t region_size = 0;
    int in_copy = 0;
    int in_region = 0;
    int all = 0;
};

void Trace(void* ctx, const tisple::Machine& m) {
    Landings& l = *static_cast<Landings*>(ctx);
    std::uint16_t pc = m.cpu.PC;
    if (pc == l.entry && l.prev != 0) {
        l.all++;
        l.in_copy += l.prev >= l.copy_start && l.prev < l.copy_end ? 1 : 0;
        l.in_region += l.prev >= l.region && l.prev < l.region + l.region_size ? 1 : 0;
    }
    l.prev = pc;
}

// interrupts every 997 cycles while groups load and run, on the emulated IIe: each one taken,
// the handler's count and table reads right, some landing in the copy and some in group code
void TestInterrupts(TestResults& results) {
    const char* text = R"(
(macro counting (n)
    `(make (array u8 ,n) ,@(map (fn (i) (% i 64)) (range 0 n))))
(let table (array u8 64) (counting 64))
(let fill (array u8 20000))
(let more (array u8 12000))
(let extra (array u8 6000))
(let count u8 :address 0x0302)
(let hits (array u16 1))
(let seen (array u16 1))
(let sum u16 0 :address 0x0300)
(let buf (array u8 600))
(fn tick () void :interrupt
    (set (at hits 0) (+ (at hits 0) 1))
    (set (at seen 0) (+ (at seen 0) (as u16 (at table (& (as u8 (at hits 0)) 63))))))
(macro stores (n k)
    (let out (list))
    (for i 0 n (set out (append out (list `(set (at buf ,(+ i k)) (+ (at buf ,(+ i k)) ,(% i 7)))))))
    `(do ,@out))
(fn one ((i u8)) u8 (stores 60 0) (+ i 1))
(fn two ((i u8)) u8 (stores 60 100) (+ i 2))
(fn three ((i u8)) u8 (stores 60 200) (+ i 3))
(fn main () void
    (set (at fill (as u16 count)) 1)
    (set (at more (as u16 count)) 2)
    (set (at extra (as u16 count)) 3)
    (for k u8 0 60
        (set sum (+ sum (as u16 (one k))))
        (set sum (+ sum (as u16 (two k))))
        (set sum (+ sum (as u16 (three k))))))
)";
    tisple::BuildOptions options;
    options.overlay_test = 1;
    options.no_inline = {"t.one", "t.two", "t.three"};
    Compiled c = CompileMainWith(text, options);
    if (!Report(c)) {
        CHECK(results, false);
        return;
    }
    CHECK(results, GroupOf(c, "t.tick") < 0);
    CHECK(results, GroupOf(c, "t.one") >= 0 && GroupOf(c, "t.two") >= 0);
    Landings l;
    l.entry = c.code.irq_direct;
    l.region = c.code.overlays.region;
    l.region_size = c.code.overlays.region_size;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.name == tisple::kOverlayCopySpan) {
            l.copy_start = c.code.as.items[span.first].address;
            l.copy_end = static_cast<std::uint16_t>(c.code.as.items[span.end - 1].address +
                                                    c.code.as.items[span.end - 1].size);
        }
    }
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    CHECK(results, RunOnMachine(c, *a, 40 * 1000 * 1000, 300, 997, Trace, &l));
    std::uint32_t want = 0;
    for (int k = 0; k < 60; k++) {
        want += static_cast<std::uint32_t>(3 * k + 6);
    }
    std::uint16_t hits = static_cast<std::uint16_t>(
        a->cpu.mem[c.code.layout
                       .vars[static_cast<std::size_t>(tisple::FindVar(c.built.program, "t.hits"))]
                       .address] |
        a->cpu.mem[c.code.layout
                       .vars[static_cast<std::size_t>(tisple::FindVar(c.built.program, "t.hits"))]
                       .address +
                   1]
            << 8);
    std::uint32_t got = static_cast<std::uint32_t>(a->cpu.mem[0x0300] | (a->cpu.mem[0x0301] << 8));
    CHECK(results, got == want);
    CHECK(results, a->cpu.irqs_taken == 300 && hits == 300 && l.all == 300);
    CHECK(results, l.in_copy > 0 && l.in_region > 0);
    std::printf("overlays: %d ticks, %d landed in the copy, %d in group code\n", l.all, l.in_copy,
                l.in_region);
    if (got != want || l.all != 300) {
        std::fprintf(stderr,
                     "overlay interrupts: sum %u (want %u), %llu taken, %u hits, card %d, "
                     "entry $%04X, region $%04X\n",
                     got, want, static_cast<unsigned long long>(a->cpu.irqs_taken), hits,
                     c.code.card_ram ? 1 : 0, l.entry, l.region);
    }
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.main", {});
}

} // namespace

void RunBackendOverlayTests(TestResults& results) {
    TestScreens(results);
    TestTakingTurns(results);
    TestKeptOut(results);
    TestInterrupts(results);
}

} // namespace tisple_tests
