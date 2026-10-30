// code in the language card's bank 1 (backend/card_bank.hpp): a program that does not fit
// main memory and the card gets its cold functions there, behind stubs; the stubs' bytes; what
// may not go there; the data its code names kept out of $D000-$DFFF; interrupts that land
// while bank 1 is in, on the emulated IIe; and the same program on the simulator and the
// interpreter

#include "backend/card_bank.hpp"
#include "machine/apple2.hpp"
#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

using tisple::MemoryBank;

bool Report(const Compiled& c) {
    if (!c.ok) {
        std::fprintf(stderr, "%s\n", c.errors.c_str());
    }
    return c.ok;
}

const tisple::MemorySegment* Bank1Segment(const Compiled& c) {
    for (const tisple::MemorySegment& s : c.code.segments) {
        if (s.bank == MemoryBank::kCardBank1) {
            return &s;
        }
    }
    return nullptr;
}

bool InBank1(const Compiled& c, const char* name) {
    std::int32_t f = tisple::FindFunction(c.built.program, name);
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.function == f && f >= 0) {
            return span.card_bank1;
        }
    }
    return false;
}

std::uint16_t VarAt(const Compiled& c, const char* name) {
    std::int32_t v = tisple::FindVar(c.built.program, name);
    return v < 0 ? 0 : c.code.layout.vars[static_cast<std::size_t>(v)].address;
}

// a byte of main memory or of the card's ram as the startup leaves it (bank 2 at $D000),
// whatever is switched in now
std::uint8_t RamByte(const tisple::Apple2& a, std::uint16_t at) {
    if (at >= 0xE000) {
        return a.lc.e000[at - 0xE000];
    }
    if (at >= 0xD000) {
        return a.lc.bank2_d000[at - 0xD000];
    }
    return a.cpu.mem[at];
}

std::uint16_t RamWord(const tisple::Apple2& a, std::uint16_t at) {
    return static_cast<std::uint16_t>(RamByte(a, at) |
                                      (RamByte(a, static_cast<std::uint16_t>(at + 1)) << 8));
}

// loads every segment and the file, then interrupts every `every` cycles from 3000 on
// (`irqs` of them), and runs to the final RTS
bool RunOnMachine(const Compiled& c, tisple::Apple2& a, std::uint64_t cycles, int irqs = 0,
                  std::uint64_t every = 0) {
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
    tisple::RunApple2(a, cycles);
    return a.stop_reason == tisple::MachineStop::kReturned;
}

// filler takes main memory and the card; six cold functions of stores each, and data read
// only by them, data read through a pointer, data main reads
std::string Crowded(int filler) {
    return "(let big (array u8 " + std::to_string(filler) + "))\n" + R"(
(let a (array u8 13000))
(macro counting (n)
    `(make (array u8 ,n) ,@(map (fn (i) (% i 251)) (range 0 n))))
(let c (array u8 9000) (counting 9000))
(let buf (array u8 1000))
(let result u16 0 :address 0x0300)
(macro stores (n k)
    (let out (list))
    (for i 0 n (set out (append out (list `(set (at buf ,(+ i k)) (+ (at c ,(* i 13)) ,i))))))
    `(do ,@out))
(fn f1 () void (stores 90 0))
(fn f2 () void (stores 90 100))
(fn f3 () void (stores 90 200))
(fn f4 () void (stores 90 300))
(fn f5 () void (stores 90 400))
(fn f6 ((p (ptr u8))) u8 (stores 90 500) (+ (load p) (at buf 3)))
(fn main () void
    (f1) (f2) (f3) (f4) (f5)
    (set (at big 7) (f6 (addr_of (at c 4321))))
    (f1) (f2) (f3) (f4) (f5)
    (set (at big 7) (+ (at big 7) (f6 (addr_of (at c 4320)))))
    (set (at a 12999) (at buf 450))
    (set result (+ (+ (as u16 (at c 8999)) (as u16 (at buf 7)))
                   (+ (as u16 (at a 12999)) (as u16 (at big 7))))))
)";
}

// a program that does not fit main memory and the card: its cold functions go to bank 1,
// and it runs as the interpreter does, on the simulator and on the emulated IIe
void TestCrowded(TestResults& results) {
    int filler = 6000;
    for (; filler <= 14000; filler += 250) {
        Compiled c = CompileMainText(Crowded(filler));
        if (c.ok && Bank1Segment(c) != nullptr) {
            break;
        }
    }
    CHECK(results, filler <= 14000);
    if (filler > 14000) {
        std::fprintf(stderr, "bank 1: no filler from 6000 to 14000 made a program that needs it\n");
        return;
    }
    Compiled c = CompileMainText(Crowded(filler));
    CheckSegmentsApart(results, c);
    const tisple::MemorySegment* bank1 = Bank1Segment(c);
    CHECK(results, bank1->address == 0xD000 && bank1->bytes.size() <= 0x1000);
    int cold = 0;
    for (const char* name : {"t.f1", "t.f2", "t.f3", "t.f4", "t.f5", "t.f6"}) {
        cold += InBank1(c, name) ? 1 : 0;
    }
    CHECK(results, cold > 0 && !InBank1(c, "t.main"));
    // the build says what it traded
    bool noted = false;
    for (const tisple::Diagnostic& w : c.code.warnings) {
        noted = noted || w.message.find("from the language card's bank 1") != std::string::npos;
    }
    CHECK(results, noted);
    // what bank 1 code reads is not in $D000-$DFFF: c directly, and through f6's pointer
    std::uint16_t at = VarAt(c, "t.c");
    CHECK(results, at + 9000 <= 0xD000 || at >= 0xE000);
    at = VarAt(c, "t.buf");
    CHECK(results, at + 1000 <= 0xD000 || at >= 0xE000);
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.main", {});
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    CHECK(results, RunOnMachine(c, *a, 4000000));
    CHECK(results,
          a->cpu.mem[0x0300] == d.in.memory[0x0300] && a->cpu.mem[0x0301] == d.in.memory[0x0301]);
    CHECK(results, !a->lc.read_ram); // the rom again after main
    std::printf("bank 1: %zu bytes of code with a filler of %d bytes\n", bank1->bytes.size(),
                filler);
}

// stubs: into bank 1 BIT $C08B / JSR / BIT $C083 / RTS, out of it the other way round
void TestStubs(TestResults& results) {
    const char* text = R"(
(let out (array u8 8))
(fn leaf ((x u8)) u8 (* x 3))
(fn mid ((x u8)) u8 (set (at out 1) (leaf x)) (+ (leaf (+ x 1)) 1))
(fn main () void
    (set (at out 0) (mid (io_read u8 0xC0F0)))
    (set (at out 2) (leaf (io_read u8 0xC0F1))))
)";
    tisple::BuildOptions options;
    options.bank1_test = 1;
    options.no_inline = {"t.leaf", "t.mid"};
    Compiled c = CompileMainWith(text, options);
    if (!Report(c)) {
        CHECK(results, false);
        return;
    }
    CHECK(results, InBank1(c, "t.mid") && InBank1(c, "t.leaf") && InBank1(c, "t.main"));
    std::string listing = tisple::ListProgram(c.built.program, c.code);
    CHECK(results, listing.find("bank stubs") != std::string::npos);
    CHECK(results, listing.find("bank1.t.main:") != std::string::npos);
    CHECK(results, listing.find("t.main  $D0") != std::string::npos);
    // the startup's JSR main goes through the stub, whose bytes are as above
    std::int32_t main = c.built.program.main;
    std::uint16_t stub = c.code.function_address[static_cast<std::size_t>(main)];
    CHECK(results, stub < 0xD000);
    std::uint16_t at = static_cast<std::uint16_t>(stub - c.code.origin);
    const std::vector<std::uint8_t>& b = c.code.image;
    CHECK(results, at + 10 <= b.size() && b[at] == 0x2C && b[at + 1] == 0x8B && b[at + 2] == 0xC0 &&
                       b[at + 3] == 0x20 && b[at + 5] == 0xD0 && b[at + 6] == 0x2C &&
                       b[at + 7] == 0x83 && b[at + 8] == 0xC0 && b[at + 9] == 0x60);
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.main", {});

    // a multiply's runtime routine stays in main memory: bank 1 calls it through a back stub
    const char* mul = R"(
(let out (array u16 4))
(fn mul ((x u16) (y u16)) u16 (* x y))
(fn main () void
    (set (at out 0) (mul (io_read u16 0xC0F0) 5678))
    (set (at out 1) (mul 77 (at out 0))))
)";
    options.no_inline = {"t.mul"};
    Compiled m = CompileMainWith(mul, options);
    CHECK(results, Report(m) && InBank1(m, "t.mul"));
    listing = tisple::ListProgram(m.built.program, m.code);
    CHECK(results, listing.find("bank2.rt.") != std::string::npos);
    Diff dm;
    CHECK(results, StartDiff(dm, m));
    CheckDiff(results, dm, "t.main", {});
}

// what never goes to bank 1: the interrupt handler and what it calls, a function pointer's
// dispatcher and entries; the functions they call do, through stubs
void TestKeptOut(TestResults& results) {
    const char* text = R"(
(let hits u8 0)
(let out (array u8 8))
(fn bump () void (set hits (+ hits 1)))
(fn tick () void :interrupt (bump))
(fn twice ((x u8)) u8 (+ x x))
(fn thrice ((x u8)) u8 (+ x (+ x x)))
(fn main () void
    (let p (fn (u8) u8) twice)
    (if (> (at out 7) 0) (set p thrice))
    (set (at out 0) (p 21))
    (set p thrice)
    (set (at out 1) (p 5)))
)";
    tisple::BuildOptions options;
    options.bank1_test = 1;
    options.no_inline = {"t.twice", "t.thrice", "t.bump"};
    Compiled c = CompileMainWith(text, options);
    if (!Report(c)) {
        CHECK(results, false);
        return;
    }
    CHECK(results, !InBank1(c, "t.tick") && !InBank1(c, "t.bump"));
    CHECK(results, InBank1(c, "t.twice") && InBank1(c, "t.thrice"));
    // the spans' functions are the backend's (dispatchers and entries added): by name
    int kept_out = 0;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        bool lowered = span.name.find(".indirect") != std::string::npos ||
                       span.name.find("dispatch") != std::string::npos;
        kept_out += lowered ? 1 : 0;
        CHECK(results, !(span.card_bank1 && lowered));
    }
    CHECK(results, kept_out >= 3);
    // the interrupt entry saves the bank: LDA $C011 after its CLD
    std::string listing = tisple::ListProgram(c.built.program, c.code);
    CHECK(results, listing.find("LDA $C011") != std::string::npos);
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.main", {});
}

// interrupts every few hundred cycles while bank 1 code runs, and in its stubs, on the
// emulated IIe: each one counted, the handler reading data in bank 2's $D000-$DFFF
void TestInterrupts(TestResults& results) {
    const char* text = R"(
(macro counting (n)
    `(make (array u8 ,n) ,@(map (fn (i) (% i 64)) (range 0 n))))
(let fill (array u8 22000))
(let more (array u8 13000))
(let table (array u8 4000) (counting 4000))
(let hits (array u16 1))
(let seen (array u16 1))
(let sum u16 0 :address 0x0300)
(let count u8 :address 0x0302)
(fn tick () void :interrupt
    (set (at hits 0) (+ (at hits 0) 1))
    (set (at seen 0) (+ (at seen 0) (as u16 (at table (as u16 (& (as u8 (at hits 0)) 63)))))))
(fn step ((i u8)) u8 (+ (* i 3) 1))
(fn walk ((n u8)) u16
    (let s u16 0)
    (for i u8 0 n (set s (+ s (as u16 (step i)))))
    s)
(fn main () void
    (set (at fill (as u16 count)) 1)
    (set (at more (as u16 count)) 2)
    (for k u8 0 200 (set sum (+ sum (walk count)))))
)";
    tisple::BuildOptions options;
    options.bank1_test = 1;
    options.no_inline = {"t.step", "t.walk"};
    Compiled c = CompileMainWith(text, options);
    if (!Report(c)) {
        CHECK(results, false);
        return;
    }
    CHECK(results, InBank1(c, "t.walk") && InBank1(c, "t.step") && !InBank1(c, "t.tick"));
    // the handler's table in bank 2's $D000-$DFFF, which a tick in bank 1 code must see
    std::uint16_t table = VarAt(c, "t.table");
    CHECK(results, table >= 0xD000 && table < 0xE000);
    if (table < 0xD000) {
        std::fprintf(stderr, "bank 1 interrupts: table at $%04X, fill at $%04X, more at $%04X\n",
                     table, VarAt(c, "t.fill"), VarAt(c, "t.more"));
    }
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    a->cpu.mem[0x0302] = 40; // the count, which the build cannot know
    CHECK(results, RunOnMachine(c, *a, 20 * 1000 * 1000, 200, 997));
    std::uint32_t want = 0;
    for (int k = 0; k < 200; k++) {
        for (int i = 0; i < 40; i++) {
            want += static_cast<std::uint32_t>(i * 3 + 1);
        }
    }
    std::uint16_t hits = RamWord(*a, VarAt(c, "t.hits"));
    std::uint32_t got = RamWord(*a, 0x0300);
    CHECK(results, got == (want & 0xFFFF));
    CHECK(results, a->cpu.irqs_taken == 200 && hits == 200);
    std::uint32_t seen = RamWord(*a, VarAt(c, "t.seen"));
    std::uint32_t want_seen = 0;
    for (int h = 1; h <= 200; h++) {
        want_seen += static_cast<std::uint32_t>(h & 63);
    }
    CHECK(results, seen == want_seen);
    if (got != (want & 0xFFFF) || hits != 200 || seen != want_seen) {
        std::fprintf(
            stderr, "bank 1 interrupts: sum %u (want %u), %llu taken, %u hits, seen %u (want %u)\n",
            got, want & 0xFFFF, static_cast<unsigned long long>(a->cpu.irqs_taken), hits, seen,
            want_seen);
    }
}

} // namespace

void RunBackendBank1Tests(TestResults& results) {
    TestCrowded(results);
    TestStubs(results);
    TestKeptOut(results);
    TestInterrupts(results);
}

} // namespace tisple_tests
