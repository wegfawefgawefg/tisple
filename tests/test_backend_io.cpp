// I/O through a base address in a global (backend/io_bases.cpp): the accesses are absolute
// instructions whose operands the base's patch function rewrites after each set, the startup
// runs it once, and the interpreter sees the same accesses; what keeps a global from being a
// base. Parameters an inlined copy hands on (backend/gen_forward.cpp), a load after an INC or
// DEC of its byte (dead_regs.cpp), a tail call over a label (peephole.cpp), the IRQ entry
// saving only the index registers the handler changes (irq.cpp), and the handler's run count.

#include "backend/analysis.hpp"
#include "emu/machine.hpp"
#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

using tisple::Mn;
using tisple::Mode;

// a function's instructions as linked
std::vector<tisple::AsmItem> Code(const Compiled& c, const std::string& name) {
    std::vector<tisple::AsmItem> out;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.name != name) {
            continue;
        }
        for (std::size_t i = span.first; i < span.end; i++) {
            if (c.code.as.items[i].kind == tisple::ItemKind::Ins) {
                out.push_back(c.code.as.items[i]);
            }
        }
    }
    return out;
}

int Bytes(const std::vector<tisple::AsmItem>& code) {
    int n = 0;
    for (const tisple::AsmItem& item : code) {
        n += item.size;
    }
    return n;
}

int Count(const std::vector<tisple::AsmItem>& code, Mn mn, Mode mode) {
    int n = 0;
    for (const tisple::AsmItem& item : code) {
        n += item.mn == mn && item.mode == mode ? 1 : 0;
    }
    return n;
}

// instructions naming a symbol called name
int Naming(const Compiled& c, const std::vector<tisple::AsmItem>& code, const std::string& name) {
    int n = 0;
    for (const tisple::AsmItem& item : code) {
        bool named = item.value.sym >= 0 &&
                     c.code.as.symbols[static_cast<std::size_t>(item.value.sym)].name == name;
        n += named ? 1 : 0;
    }
    return n;
}

// instructions naming a symbol called name, in every function's code
int NamingAll(const Compiled& c, const std::string& name) {
    int n = 0;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.function >= 0) {
            n += Naming(c, Code(c, span.name), name);
        }
    }
    return n;
}

// main on both, the program printed on a difference
void CheckSame(TestResults& results, const Compiled& c, const char* text) {
    Diff d;
    bool ok = StartDiff(d, c) && DiffCall(d, "t.main", {});
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text, d.message.c_str());
    }
    CHECK(results, ok);
}

// an AY register write through a 6522 at page, page set three times
const char* kWrite = R"(
(let page u16 0xC400)
(let wide u16 0)
(fn write ((reg u8) (value u8)) void
    (io_write u8 (+ page 1) reg)
    (io_write u8 page 7)
    (io_write u8 page 4)
    (io_write u8 (+ page 1) value)
    (io_write u8 page 6)
    (io_write u8 page 4))
(fn move ((slot u8)) void
    (set page (+ 0xC000 (<< (as u16 slot) 8))))
(fn main () void
    (write 3 9)
    (move 5)
    (write 5 200)
    (set page (+ page 0x80))
    (write 1 2)
    (set wide (io_read u16 (- page 0x7C)))
    (io_write u16 (+ 0x10 page) wide))
)";

void WriteTests(TestResults& results) {
    Compiled c = CompileText(kWrite);
    CHECK(results, c.ok);
    if (!c.ok) {
        return;
    }
    // LDA reg / STA / LDA #7 / STA / LDA #4 / STA / LDA value / STA / LDA #6 / STA / LDA #4 /
    // STA / RTS: no address worked out, every access absolute
    std::vector<tisple::AsmItem> write = Code(c, "t.write");
    CHECK(results, Bytes(write) == 31);
    CHECK(results, Count(write, Mn::STA, Mode::Abs) == 6);
    CHECK(results, write.size() == 13);
    CHECK(results, Naming(c, write, "t.page.io") == 6);
    // a call of write: 44 cycles in it (6 STA absolute, 2 LDA zero page, 4 LDA #, RTS)
    Diff d;
    bool ok = StartDiff(d, c) && DiffCall(d, "t.write", {3, 9});
    CHECK(results, ok);
    std::uint64_t cycles = d.last_cycles;
    CHECK(results, ok && cycles == 44);
    // the six accesses at $C401 $C400 $C400 $C401 $C400 $C400
    CHECK(results, ok && d.sim.io.size() == 6 && d.sim.io[0].address == 0xC401 &&
                       d.sim.io[1].address == 0xC400 && d.sim.io[3].value == 9);
    // the sets call the patch (move's in main's copy of it too), the startup once; with no
    // handler it leaves the I flag alone
    CHECK(results, Naming(c, Code(c, "t.move"), "t.page.patch") == 1);
    CHECK(results, Naming(c, Code(c, "t.main"), "t.page.patch") == 2);
    CHECK(results, Naming(c, Code(c, "startup"), "t.page.patch") == 1);
    std::vector<tisple::AsmItem> patch = Code(c, "t.page.patch");
    CHECK(results, Count(patch, Mn::SEI, Mode::Imp) == 0 && Count(patch, Mn::PHP, Mode::Imp) == 0);
    // two stores for each access, wherever it is (copies of write in main, the u16 ones)
    int sites = NamingAll(c, "t.page.io");
    CHECK(results, sites >= 6 + 4);
    CHECK(results, Count(patch, Mn::STA, Mode::Abs) == 2 * sites);
    // the interpreter's accesses: $C401..., $C501..., $C581..., the u16 read and write
    CheckSame(results, c, kWrite);
    // and with no mid-level passes
    tisple::OptOptions o0;
    o0.level = 0;
    CheckSame(results, CompileMainText(kWrite, o0), kWrite);
}

// not a base: its address taken, set in the handler, its value changed by what an io_write
// works out after the address
const char* kNotBase = R"(
(let page u16 0xC400)
(let mine u16 0xC300)
(let other u16 0xC200)
(fn irq () void :interrupt
    (set mine (+ mine 1)))
(fn bump () u8
    (set other (+ other 0x10))
    3)
(fn main () void
    (store (addr_of page) 0xC500)
    (io_write u8 (+ page 1) 5)
    (io_write u8 mine 6)
    (io_write u8 (+ other 2) (bump))
    (io_write u8 (+ other 2) 7))
)";

void NotBaseTests(TestResults& results) {
    Compiled c = CompileText(kNotBase);
    CHECK(results, c.ok);
    if (!c.ok) {
        return;
    }
    std::vector<tisple::AsmItem> main = Code(c, "t.main");
    CHECK(results, Naming(c, main, "t.page.io") == 0);
    CHECK(results, Naming(c, main, "t.mine.io") == 0);
    // other is a base, but not where bump, worked out after the address, sets it
    CHECK(results, Naming(c, main, "t.other.io") == 1);
    Diff d;
    bool ok = StartDiff(d, c) && DiffCall(d, "t.main", {});
    CHECK(results, ok);
    CHECK(results, ok && d.sim.io.size() == 4 && d.sim.io[0].address == 0xC501 &&
                       d.sim.io[2].address == 0xC202 && d.sim.io[3].address == 0xC212);
}

// the handler reads through a base main sets: the patch keeps interrupts off while it writes
// the handler's operands
const char* kHandler = R"(
(let page u16 0xC400)
(let seen u8 0)
(let ticks u8 0)
(fn irq () void :interrupt
    (set ticks (+ ticks 1))
    (set seen (io_read u8 (+ page 0x0D))))
(fn main () void
    (sei)
    (set page 0xC600)
    (cli)
    (for i u8 0 200 (wait_cycles 10)))
)";

void HandlerTests(TestResults& results) {
    Compiled c = CompileMainText(kHandler);
    CHECK(results, c.ok);
    if (!c.ok) {
        return;
    }
    std::vector<tisple::AsmItem> patch = Code(c, "t.page.patch");
    CHECK(results, Count(patch, Mn::PHP, Mode::Imp) == 1 && Count(patch, Mn::SEI, Mode::Imp) == 1 &&
                       Count(patch, Mn::PLP, Mode::Imp) == 1);
    CHECK(results, Naming(c, Code(c, "t.irq"), "t.page.io") == 1);
    // an interrupt late in main reads $C60D
    Diff d;
    bool ok = StartDiff(d, c);
    tisple::Machine& m = *d.sim.machine;
    m.mem[0xFFFE] = static_cast<std::uint8_t>(c.code.irq_direct & 0xFF);
    m.mem[0xFFFF] = static_cast<std::uint8_t>(c.code.irq_direct >> 8);
    tisple::RequestIrq(m, m.cycles + 2000);
    tisple::SimCall got =
        tisple::CallCompiled(d.sim, c.built.program, c.code,
                             tisple::FindFunction(c.built.program, "t.main"), {}, 1000000);
    CHECK(results, ok && got.returned && m.irqs_taken == 1);
    bool read = false;
    for (const tisple::IoAccess& a : d.sim.io) {
        read = read || (a.address == 0xC60D && !a.write);
    }
    CHECK(results, read);
}

// reg, inlined, keeps i and value in send's parameters: no copies before the JSR
const char* kForward = R"(
(let sent (array u8 8))
(let log (array u8 8))
(fn send ((i u8) (value u8)) void
    (set (at sent i) value)
    (for k u8 0 3
        (set (at log (& (+ i k) 7)) (+ value k))))
(fn reg ((i u8) (value u8)) void
    (if (!= (at sent i) value)
        (send i value)))
(fn twice ((i u8) (value u8)) void
    (send i value)
    (send (+ i 1) value))
(fn main () void
    (reg (& (at log 3) 7) (+ (at sent 2) 5))
    (reg 1 (at log 1))
    (reg (& (at log 3) 7) (+ (at sent 2) 5))
    (twice (& (at log 4) 6) 9))
)";

void ForwardTests(TestResults& results) {
    tisple::OptOptions o0;
    o0.level = 0;
    Compiled c = CompileMainText(kForward, o0);
    CHECK(results, c.ok);
    if (!c.ok) {
        return;
    }
    std::vector<tisple::AsmItem> main = Code(c, "t.main");
    CHECK(results, Naming(c, main, "t.reg.i") == 0 && Naming(c, main, "t.reg.value") == 0);
    // twice calls send twice: its parameters stay its own
    CHECK(results, Naming(c, main, "t.twice.i") > 0);
    CheckSame(results, c, kForward);
    CheckSame(results, CompileMainText(kForward), kForward);
}

// a countdown: DEC, then the branch on its flags, no load of the byte again (A is set
// before anything reads it)
const char* kCount = R"(
(let wait (array u8 4) (make (array u8 4) 3 1 9 2))
(let fired u8 0)
(let last u8 0)
(fn step ((s u8)) void
    (set (at wait s) (- (at wait s) 1))
    (if (= (at wait s) 0)
        (set fired (+ fired 1)))
    (set last 7))
(fn main () void
    (for t u8 0 12
        (for s u8 0 4
            (step s))))
)";

// a tail call over a label: JMP, no JSR and RTS
const char* kTail = R"(
(let n u8 0)
(let m (array u8 16))
(fn big ((x u8)) void
    (for k u8 0 x
        (set n (+ n (* k 3)))
        (set (at m (& (+ k n) 15)) (^ (at m (& k 15)) n))
        (set (at m (& (- k n) 15)) (+ (at m (& n 15)) k))))
(fn maybe ((x u8)) void
    (if (!= x 0)
        (big x)))
(fn main () void
    (maybe n)
    (big 2)
    (maybe 4))
)";

void PeepTests(TestResults& results) {
    Compiled c = CompileText(kCount);
    CHECK(results, c.ok);
    std::vector<tisple::AsmItem> step = Code(c, "t.step");
    CHECK(results, Count(step, Mn::DEC, Mode::AbsX) == 1);
    CHECK(results, Count(step, Mn::LDA, Mode::AbsX) == 0);
    CheckSame(results, c, kCount);
    tisple::OptOptions o0;
    o0.level = 0;
    Compiled t = CompileText(kTail);
    CHECK(results, t.ok);
    std::vector<tisple::AsmItem> maybe = Code(t, "t.maybe");
    CHECK(results, Naming(t, maybe, "t.big") == 1 && Count(maybe, Mn::JMP, Mode::Abs) == 1 &&
                       Count(maybe, Mn::JSR, Mode::Abs) == 0);
    CheckSame(results, t, kTail);
}

// the entry saves X only: the handler's code never changes Y
const char* kNoY = R"(
(let counts (array u8 4))
(let ticks u8 0)
(fn irq () void :interrupt
    (set ticks (+ ticks 1))
    (set (at counts (& ticks 3)) (+ (at counts (& ticks 3)) 1)))
(fn main () void
    (for i u8 0 100 (wait_cycles 10)))
)";

void EntryTests(TestResults& results) {
    Compiled c = CompileMainText(kNoY);
    CHECK(results, c.ok);
    if (!c.ok) {
        return;
    }
    std::vector<tisple::AsmItem> entry = Code(c, "interrupt entry");
    CHECK(results, Count(entry, Mn::TXA, Mode::Imp) == 1 && Count(entry, Mn::TAX, Mode::Imp) == 1);
    CHECK(results, Count(entry, Mn::TYA, Mode::Imp) == 0 && Count(entry, Mn::TAY, Mode::Imp) == 0);
    // STA $45 CLD TXA PHA JSR PLA TAX LDA $45 RTI: 13 bytes (and the saved vector after)
    CHECK(results, Bytes(entry) == 13);
    // the handler runs as often as a while loop's body, its callees with it
    std::vector<std::uint64_t> runs = tisple::FindRunCounts(c.built.program);
    std::int32_t irq = tisple::FindFunction(c.built.program, "t.irq");
    CHECK(results, irq >= 0 && runs[static_cast<std::size_t>(irq)] == 8);
}

} // namespace

void RunBackendIoTests(TestResults& results) {
    WriteTests(results);
    NotBaseTests(results);
    HandlerTests(results);
    ForwardTests(results);
    PeepTests(results);
    EntryTests(results);
}

} // namespace tisple_tests
