// data in the language card's bank 1 (backend/card_data.hpp): lets :in bank1, the checker's
// rules for them, where the link puts them (the bank's top, its code below), the code that
// switches the card itself (kept out of $D000-$DFFF and bank 1, the interrupt entry that puts
// bank 2 in), the same program on the interpreter and the simulator; then on the emulated IIe
// with the card: a song played from bank 1 (mockingboard.play_bank1, service_bank1) sounds as
// the same song from main memory, tick for tick, and a table read from the disk into bank 1
// (disk.read_card_with) while a song plays from there, no tick lost

#include "disk/disk_reader.hpp"
#include "machine/mockingboard.hpp"
#include "machine/tick_log.hpp"
#include "test_backend.hpp"
#include "test_check.hpp"
#include "test_disk.hpp"
#include "test_disk_read.hpp"
#include "test_mockingboard.hpp"

#include <cstdio>
#include <cstring>

namespace tisple_tests {

namespace {

void CheckerTests(TestResults& results) {
    ExpectError(results, "(let room (array u8 2) (make (array u8 2) 1 2) :in bank1)",
                "t.tsp:1:1: room is :in bank1, which the startup never writes, so it has no "
                "value; fill it while the program runs (lib/disk reads into it)");
    ExpectError(results, "(const T (array u8 2) (make (array u8 2) 1 2) :in bank1)",
                "t.tsp:1:47: :in bank1 is for lets with no value, filled while the program "
                "runs; make T a let, or put the table :in aux");
    ExpectError(results, "(let room (array u8 16) :in bank1 :address 0xD800)",
                "t.tsp:1:1: room is :in bank1, placed by the link at the top of the bank; drop "
                ":address, :align, :boundary and :reserve");
    ExpectError(results, "(let room (array u8 5000) :in bank1)",
                "t.tsp:1:1: room is bigger than the language card's bank 1 (4096 bytes)");
    ExpectError(results, "(let room (array u8 16) :in bank1 :in aux)",
                "t.tsp:1:35: :in is given twice; keep one");
    ExpectError(results, "(let room (array u8 16) :in bank1)\n(fn f () void (set (at room 0) 1))",
                "t.tsp:2:24: room is in the language card's bank 1, which code reaches only "
                "through pointers with bank 1 switched in: (addr_of (at room 0)), for lib/disk "
                "or lib/mockingboard");
}

uint16_t VarAt(const Compiled& c, const char* name) {
    int32_t v = tisple::FindVar(c.built.program, name);
    return v < 0 ? 0 : c.code.layout.vars[static_cast<size_t>(v)].address;
}

uint16_t FunctionAt(const Compiled& c, const char* name) {
    int32_t f = tisple::FindFunction(c.built.program, name);
    return f < 0 ? 0 : c.code.function_address[static_cast<size_t>(f)];
}

bool InD000(uint16_t at) {
    return at >= 0xD000 && at < 0xE000;
}

// two lets at the bank's top, the first lowest; the code that switches bank 1 in itself (put
// and sum, which call nothing between their switches; sum calls add while bank 1 is in, so add
// goes with them) out of $D000-$DFFF and out of bank 1 though every other function is put
// there; the same results on the interpreter and the simulator
const char* kSwitching = R"(
(let room (array u8 300) :in bank1)
(let more (array u8 40) :in bank1)
(let total u16 0)
(let tally (array u8 4))
(fn add ((x u8)) void
    (set (at tally 0) (+ (at tally 0) x)))
(fn put ((p (ptr u8)) (n u8)) void
    (for i u8 0 n
        (io_read u8 0xC08B)
        (store (+ p i) (+ i 7))
        (io_read u8 0xC083)))
(fn sum ((p (ptr u8)) (n u8)) u16
    (let s u16 0)
    (io_read u8 0xC08B)
    (for i u8 0 n
        (set s (+ s (as u16 (load (+ p i)))))
        (add (load (+ p i))))
    (io_read u8 0xC083)
    s)
(fn other ((x u8)) u8 (+ x 1))
(fn main () void
    (put (addr_of (at room 0)) 200)
    (put (addr_of (at more 0)) 40)
    (set total (+ (sum (addr_of (at room 0)) 200) (sum (addr_of (at more 0)) 40)))
    (set (at tally 1) (other (at tally 0))))
)";

void LinkTests(TestResults& results) {
    tisple::BuildOptions options;
    options.bank1_test = 1;
    options.no_inline = {"t.add", "t.put", "t.sum", "t.other"};
    Compiled c = CompileMainWith(kSwitching, options);
    CHECK(results, c.ok);
    if (!c.ok) {
        std::fprintf(stderr, "%s\n", c.errors.c_str());
        return;
    }
    CHECK(results, c.code.card_ram);
    CHECK(results, VarAt(c, "t.room") == 0xE000 - 340 && VarAt(c, "t.more") == 0xE000 - 40);
    // bank 1 holds code only below the lets, and none of the switching code
    uint32_t code_end = 0xD000;
    for (const tisple::MemorySegment& s : c.code.segments) {
        if (s.bank == tisple::MemoryBank::kCardBank1) {
            code_end = s.address + static_cast<uint32_t>(s.bytes.size());
        }
    }
    CHECK(results, code_end <= 0xE000 - 340);
    bool other_in_bank1 = false;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.function == tisple::FindFunction(c.built.program, "t.other")) {
            other_in_bank1 = span.card_bank1;
        }
    }
    CHECK(results, other_in_bank1); // the test puts what may go there in bank 1
    for (const char* name : {"t.put", "t.sum", "t.add"}) {
        uint16_t at = FunctionAt(c, name);
        CHECK(results, at != 0 && !InD000(at));
    }
    // what they name stays out of $D000-$DFFF
    CHECK(results, !InD000(VarAt(c, "t.tally")) && !InD000(VarAt(c, "t.total")));
    // the interpreter and the simulator both switch on the program's own reads of $C08B/$C083
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.main", {});
    // 7 + ... + 206 for room, 7 + ... + 46 for more
    uint16_t want = static_cast<uint16_t>((7 + 206) * 200 / 2 + (7 + 46) * 40 / 2);
    uint16_t at = VarAt(c, "t.total");
    CHECK(results, (d.in.memory[at] | (d.in.memory[at + 1] << 8)) == want);
}

// a song from main memory or from bank 1, the same voices (and an effect from main memory on
// top), and the ticks' cost each way
const char* kSong = R"(
(import mockingboard mb)
(import mockingboard_song tune)
(tune.song busy
    (pattern bass "l9 v12 k3 a2 - - r e2 r a2 - r g2 r r")
    (pattern ma "l9 v10 k1 a4 e5 b4 r d5 c5 r e5 g4 r")
    (pattern pad "l54 v7 k12 c4 - e4 - c4 - g3 -")
    (pattern sh "l9 m2 k1 n1 v5 c4 v3 c4 c4")
    (pattern bell "l7 v9 k1 b5 r e6 r d6")
    (pattern env "l18 h8:300 c3 r")
    (voice 0 bass) (voice 1 ma) (voice 2 pad) (voice 3 sh) (voice 4 bell) (voice 5 env))
(tune.effect zap "l2 v15 k1 c6 g5 c5 g4 c4")
(let room (array u8 700) :in bank1)
(fn irq () void :interrupt (SERVICE))
(fn copy_in () void
    (let from (ptr u8) (addr_of (at busy 0)))
    (let to (ptr u8) (addr_of (at room 300)))
    (for i u16 0 400
        (let b u8 (load (+ from i)))
        (io_read u8 0xC08B)
        (store (+ to i) b)
        (io_read u8 0xC083)))
(fn main () void
    (mb.init)
    (copy_in)
    (PLAY)
    (for i u8 0 60
        (wait_cycles 17030))
    (mb.effect (addr_of (at zap 0)) 2 5)
    (for i u8 0 140
        (wait_cycles 17030))
    (mb.off))
)";

std::string SongProgram(bool bank1) {
    std::string text = kSong;
    auto put = [&](const std::string& mark, const std::string& with) {
        text.replace(text.find(mark), mark.size(), with);
    };
    put("(SERVICE)", bank1 ? "(mb.service_bank1)" : "(mb.service)");
    put("(PLAY)",
        bank1 ? "(mb.play_bank1 (addr_of (at room 300)))" : "(mb.play (addr_of (at busy 0)))");
    return text;
}

struct SongRun {
    bool ok = false;
    Voices voices;
    TickCost cost;
    uint16_t room = 0;
    std::vector<uint8_t> bank1;
    std::vector<uint8_t> bank2;
    std::vector<uint8_t> song;
};

SongRun RunSong(TestResults& results, bool bank1) {
    SongRun r;
    MbBuild b;
    MbBuildText(results, "t.tsp", SongProgram(bank1), b);
    if (!b.ok) {
        return r;
    }
    std::unique_ptr<tisple::Apple2> a = MbStart(b, 4);
    r.cost.entry = b.code.irq_direct;
    tisple::RunApple2Traced(*a, 230 * kMbTick, MeasureTick, &r.cost);
    r.ok = a->stop_reason == tisple::MachineStop::kReturned;
    r.voices = ReadVoices(*a, FirstTick(*a), 190);
    r.room = MbVarAddress(b, "t.room");
    if (r.room >= 0xD000 && r.room + 700 <= 0xE000) {
        size_t at = r.room - 0xD000u;
        r.bank1.assign(a->lc.bank1_d000 + at, a->lc.bank1_d000 + at + 700);
        r.bank2.assign(a->lc.bank2_d000 + at, a->lc.bank2_d000 + at + 700);
    }
    // the song's bytes where the startup left them: main memory, or the card's ram (bank 2)
    uint16_t busy = MbVarAddress(b, "t.busy");
    int32_t v = tisple::FindVar(b.program, "t.busy");
    uint32_t size = v < 0 ? 0 : b.code.layout.vars[static_cast<size_t>(v)].size;
    for (uint32_t i = 0; i < size && i < 400; i++) {
        uint32_t at = busy + i;
        r.song.push_back(at >= 0xE000   ? a->lc.e000[at - 0xE000]
                         : at >= 0xD000 ? a->lc.bank2_d000[at - 0xD000]
                                        : a->cpu.mem[at]);
    }
    return r;
}

void SongTests(TestResults& results) {
    SongRun main = RunSong(results, false);
    SongRun card = RunSong(results, true);
    CHECK(results, main.ok && card.ok);
    if (!main.ok || !card.ok) {
        return;
    }
    // the song is in bank 1 at room + 300, and bank 2 there was never written
    CHECK(results, card.room == 0xE000 - 700 && card.bank1.size() == 700);
    if (card.bank1.size() != 700) {
        return;
    }
    bool copied = card.song.size() > 60 &&
                  std::memcmp(card.bank1.data() + 300, card.song.data(), card.song.size()) == 0;
    CHECK(results, copied);
    if (!copied) {
        for (size_t i = 0; i < card.song.size(); i++) {
            if (card.bank1[300 + i] != card.song[i]) {
                std::fprintf(stderr, "bank 1 song byte %zu: %02X, the song's %02X\n", i,
                             card.bank1[300 + i], card.song[i]);
                break;
            }
        }
    }
    bool bank2_clean = true;
    for (uint8_t v : card.bank2) {
        bank2_clean = bank2_clean && v == 0;
    }
    CHECK(results, bank2_clean);
    // tick for tick the same voices (the effect on voice 2 included)
    bool same = true;
    for (int v = 0; v < 6; v++) {
        same = same && main.voices.period[v] == card.voices.period[v] &&
               main.voices.volume[v] == card.voices.volume[v];
    }
    same = same && main.voices.mixer[0] == card.voices.mixer[0] &&
           main.voices.mixer[1] == card.voices.mixer[1];
    CHECK(results, same);
    bool zapped = false;
    for (size_t t = 0; t < card.voices.volume[2].size(); t++) {
        zapped = zapped || card.voices.volume[2][t] == 15;
    }
    CHECK(results, zapped);
    // a tick from bank 1 costs a few switches more, never a lot more
    double main_avg = static_cast<double>(main.cost.total) / static_cast<double>(main.cost.count);
    double card_avg = static_cast<double>(card.cost.total) / static_cast<double>(card.cost.count);
    CHECK(results, main.cost.count > 150 && card.cost.count > 150);
    CHECK(results, card_avg < main_avg + 120 && card.cost.most < main.cost.most + 300);
    std::printf("a song's tick: from main memory %.0f cycles (the most %llu), from bank 1 %.0f "
                "(the most %llu)\n",
                main_avg, static_cast<unsigned long long>(main.cost.most), card_avg,
                static_cast<unsigned long long>(card.cost.most));
}

// a table read from the disk into bank 1 while a song plays from there
std::string ReadProgram(const std::vector<uint8_t>& table) {
    return "(import disk)\n"
           "(import mockingboard mb)\n"
           "(import mockingboard_song tune)\n"
           "(tune.song busy\n"
           "    (pattern bass \"l9 v12 k3 a2 - - r e2 r a2 - r g2 r r\")\n"
           "    (pattern ma \"l9 v10 k1 a4 e5 b4 r d5 c5 r e5 g4 r\")\n"
           "    (pattern mb \"l9 v9 k1 e5 r a5 g5 r e5 d5 r c5\")\n"
           "    (pattern sp \"l9 v6 k1 e6 r r b5 r a5 r\")\n"
           "    (pattern pad \"l54 v7 k12 c4 - e4 - c4 - g3 -\")\n"
           "    (pattern sh \"l9 m2 k1 n1 v5 c4 v3 c4 c4\")\n"
           "    (voice 0 bass) (voice 1 ma) (voice 2 pad) (voice 3 mb) (voice 4 sp) (voice 5 "
           "sh))\n"
           "(const BIG (array u8 1536) " +
           MakeBytes(table) +
           " :on disk)\n"
           "(let room (array u8 2000) :in bank1)\n"
           "(let status u8 0xEE :address 0x03C2)\n"
           "(fn irq () void :interrupt\n"
           "    (mb.service_bank1))\n"
           "(fn step () void\n"
           "    (wait_cycles 1500))\n"
           "(fn copy_in () void\n"
           "    (let from (ptr u8) (addr_of (at busy 0)))\n"
           "    (let to (ptr u8) (addr_of (at room 1600)))\n"
           "    (for i u16 0 400\n"
           "        (let b u8 (load (+ from i)))\n"
           "        (io_read u8 0xC08B)\n"
           "        (store (+ to i) b)\n"
           "        (io_read u8 0xC083)))\n"
           "(fn main () void\n"
           "    (mb.init)\n"
           "    (copy_in)\n"
           "    (mb.play_bank1 (addr_of (at room 1600)))\n"
           "    (disk.read_card (addr_of (at room 0)) (addr_of (at BIG 0)) disk.BANK1) ; the "
           "head there\n"
           "    (set status (disk.read_card_with (addr_of (at room 0)) (addr_of (at BIG 0)) "
           "disk.BANK1 step))\n"
           "    (mb.off))\n";
}

struct ReadWatch {
    uint16_t begin = 0;
    std::vector<uint64_t> begins;
    uint64_t end = 0;
    tisple::TickLog* ticks = nullptr;
};

void WatchRead(void* ctx, const tisple::Machine& m) {
    ReadWatch& w = *static_cast<ReadWatch*>(ctx);
    if (tisple::GetRegs(m).pc == w.begin) {
        w.begins.push_back(m.cycles);
    }
    w.end = m.cycles;
    tisple::TickLogStep(w.ticks, m);
}

void ReadTests(TestResults& results) {
    std::vector<uint8_t> table = DiskPattern(1536, 31);
    tisple::BuildOptions options = DiskReadLayout(0x6000);
    DiskProgram p;
    BuildDiskProgram(results, ReadProgram(table), options, p);
    if (!p.ok) {
        return;
    }
    std::unique_ptr<tisple::Apple2> a = BootDiskProgram(p);
    tisple::InsertMockingboard(*a, 4);
    tisple::TickLog log;
    tisple::StartTickLog(log, *a);
    ReadWatch w;
    w.begin = 0x1700;
    w.ticks = &log;
    uint64_t start = tisple::Cycles(*a);
    while (!a->stopped && tisple::Cycles(*a) - start < kDiskReadBudget) {
        tisple::RunApple2Traced(*a, tisple::kCyclesPerFrame, WatchRead, &w);
    }
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, a->cpu.mem[0x03C2] == tisple::kReaderDone && w.begins.size() == 2);
    uint16_t room = DiskProgramVar(p, "t.room");
    CHECK(results, room == 0xE000 - 2000);
    if (room != 0xE000 - 2000 || w.begins.size() != 2) {
        return;
    }
    size_t at = room - 0xD000u;
    CHECK(results, std::memcmp(a->lc.bank1_d000 + at, table.data(), table.size()) == 0);
    bool bank2_clean = true;
    for (size_t i = 0; i < 2000; i++) {
        bank2_clean = bank2_clean && a->lc.bank2_d000[at + i] == 0;
    }
    CHECK(results, bank2_clean);
    // a tick for every 17,030 cycles of the read, none lost
    uint64_t from = w.begins[1];
    uint64_t taken = 0;
    uint64_t latest = 0;
    for (const tisple::TickEntry& e : log.ticks) {
        if (e.taken > from && e.taken < w.end) {
            taken++;
            latest = e.late > latest ? e.late : latest;
        }
    }
    uint64_t due = (w.end - from) / 17030;
    CHECK(results, taken + 1 >= due && latest < 17030);
    std::printf("a read into bank 1 under a song from there: %.3f s, %llu ticks of %llu due, the "
                "latest %llu cycles late\n",
                static_cast<double>(w.end - from) / 1023000.0,
                static_cast<unsigned long long>(taken), static_cast<unsigned long long>(due),
                static_cast<unsigned long long>(latest));
}

} // namespace

void RunBank1DataTests(TestResults& results) {
    CheckerTests(results);
    LinkTests(results);
    SongTests(results);
    ReadTests(results);
}

} // namespace tisple_tests
