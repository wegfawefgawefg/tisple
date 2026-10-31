// with_aux on the built-in IIe against the interpreter: each program runs on the machine (its
// aux segments loaded, so the mirrored code and const data are there) and in the interpreter
// on the same layout, and every global outside zero page, in main memory and aux memory, must
// come out the same; the switches end off. Constant and run-time banks, early returns, breaks
// and continues that leave a region and ones that stay in it, a 16-bit value out of one, an
// aux-mode function reading a mirrored table; then the mirrored bytes themselves, a frame too
// big for zero page, and the warning for a handler without auxmem.guard.

#include "test_backend.hpp"
#include "test_machine.hpp"

#include <algorithm>
#include <cstdio>

namespace tisple_tests {

namespace {

struct AuxRun {
    tisple::Program program;
    tisple::Compiled code;
    std::unique_ptr<tisple::Apple2> machine;
    tisple::Interp in;
    bool ok = false;
};

// compiled as tisple run does (the real lib/), run on the machine to its return and in the
// interpreter
bool RunBoth(TestResults& results, const std::string& text, AuxRun& r,
             const tisple::BuildOptions& options = tisple::BuildOptions()) {
    tisple::LoadOptions load;
    load.files.push_back(tisple::VirtualFile{"t.tsp", text});
    std::vector<tisple::Diagnostic> errors;
    bool built = tisple::BuildProgram("t.tsp", load, r.program, errors) &&
                 tisple::CompileProgram(r.program, options, r.code, errors);
    CHECK(results, built);
    if (!built) {
        std::fprintf(stderr, "%s\n%s", text.c_str(),
                     tisple::FormatDiagnostics(r.program.sources, errors).c_str());
        return false;
    }
    r.machine = tisple::NewApple2();
    std::string error;
    for (const tisple::MemorySegment& s : r.code.segments) {
        CHECK(results, tisple::LoadSegment(*r.machine, s, error));
    }
    CHECK(results, tisple::LoadProgram(*r.machine, r.code.image.data(), r.code.image.size(),
                                       r.code.origin, error));
    tisple::RunApple2(*r.machine, 20 * 1000 * 1000);
    CHECK(results, r.machine->stop_reason == tisple::MachineStop::kReturned);
    tisple::InitInterp(r.in, r.program, r.code.layout);
    tisple::InterpResult done = tisple::CallFunction(r.in, r.program.main, {});
    CHECK(results, done.kind == tisple::StopKind::Finished);
    r.ok = r.machine->stop_reason == tisple::MachineStop::kReturned &&
           done.kind == tisple::StopKind::Finished;
    return r.ok;
}

// every global's bytes outside zero page (the startup gives zero page back), in its memory
void CheckSame(TestResults& results, const std::string& text, const AuxRun& r) {
    bool same = true;
    for (std::size_t v = 0; v < r.program.vars.size() && same; v++) {
        const tisple::Var& var = r.program.vars[v];
        const tisple::VarPlace& place = r.code.layout.vars[v];
        if (var.kind != tisple::VarKind::Global || var.placement.reserve || place.dropped ||
            place.interp_only || place.address < 0x0200) {
            continue;
        }
        bool aux = var.placement.in_aux;
        for (std::uint32_t k = 0; k < place.size && same; k++) {
            std::uint16_t at = static_cast<std::uint16_t>(place.address + k);
            std::uint8_t machine = aux ? r.machine->aux[at] : r.machine->cpu.mem[at];
            std::uint8_t interp = aux ? r.in.aux[at] : r.in.memory[at];
            if (machine != interp) {
                std::fprintf(stderr, "%s\n  %s byte %u: machine %u, interpreter %u\n", text.c_str(),
                             var.name.c_str(), k, machine, interp);
                same = false;
            }
        }
    }
    CHECK(results, same);
    CHECK(results, !r.machine->sw.ramrd && !r.machine->sw.ramwrt);
    CHECK(results, !r.in.aux_reads && !r.in.aux_writes);
}

void CheckProgramBoth(TestResults& results, const std::string& text) {
    AuxRun r;
    if (RunBoth(results, text, r)) {
        CheckSame(results, text, r);
    }
}

const char* kBufs = "(macro counting (n)\n"
                    "    `(make (array u8 ,n) ,@(map (fn (i) (% (* i 7) 251)) (range 0 n))))\n"
                    "(let book (array u8 256) (counting 256) :in aux)\n"
                    "(let spare (array u8 256) :in aux)\n"
                    "(let line (array u8 256))\n"
                    "(let out (array u8 16))\n";

void ConstantBanks(TestResults& results) {
    CheckProgramBoth(
        results, std::string(kBufs) +
                     "(fn main () void\n"
                     "    (with_aux true false\n"
                     "        (for i u8 0 255\n"
                     "            (store (addr_of (at line i)) (load (addr_of (at book i))))))\n"
                     "    (with_aux false true\n"
                     "        (for i u8 0 150\n"
                     "            (store (addr_of (at spare i)) (+ (at line i) 1))))\n"
                     "    (with_aux true true\n"
                     "        (for i u8 0 100\n"
                     "            (store (addr_of (at spare (+ i 150)))\n"
                     "                (load (addr_of (at spare i)))))))");
}

void RunTimeBanks(TestResults& results) {
    CheckProgramBoth(results, std::string(kBufs) +
                                  "(fn put ((aux bool) (p (ptr u8)) (v u8)) void\n"
                                  "    (with_aux aux aux (store p (+ (load p) v))))\n"
                                  "(fn get ((reads bool) (p (ptr u8))) u8\n"
                                  "    (with_aux reads false (load p)))\n"
                                  "(fn main () void\n"
                                  "    (for i u8 0 16\n"
                                  "        (let aux bool (= (& i 1) 0))\n"
                                  "        (put aux (addr_of (at book i)) 3)\n"
                                  "        (put aux (addr_of (at line i)) 5)\n"
                                  "        (set (at out i) (+ (get true (addr_of (at book i)))\n"
                                  "            (get false (addr_of (at line i)))))))");
}

// returns, breaks and continues that leave the region, and ones whose loop is inside it
void Exits(TestResults& results) {
    CheckProgramBoth(
        results, std::string(kBufs) +
                     "(fn find ((v u8)) u8\n"
                     "    (with_aux true false\n"
                     "        (for i u8 0 255\n"
                     "            (if (= (load (addr_of (at book i))) v)\n"
                     "                (return i))))\n"
                     "    255)\n"
                     "(fn skip_odd () void\n"
                     "    (for i u8 0 16\n"
                     "        (with_aux true false\n"
                     "            (if (!= (& i 1) 0)\n"
                     "                (continue))\n"
                     "            (if (> i 11)\n"
                     "                (break))\n"
                     "            (store (addr_of (at out i)) (load (addr_of (at book i)))))))\n"
                     "(fn inside () u8\n"
                     "    (with_aux true true\n"
                     "        (let n u8 0)\n"
                     "        (for i u8 0 50\n"
                     "            (if (= (& i 3) 1)\n"
                     "                (continue))\n"
                     "            (if (= i 40)\n"
                     "                (break))\n"
                     "            (store (addr_of (at spare i)) (load (addr_of (at book i))))\n"
                     "            (set n (+ n 1)))\n"
                     "        n))\n"
                     "(fn main () void\n"
                     "    (set (at out 15) (find 35))\n"
                     "    (set (at out 14) (find 1))\n"
                     "    (skip_odd)\n"
                     "    (set (at out 13) (inside)))");
}

// a u16 out of a region, and an aux-mode function reading a const table (which the link
// mirrors) and calling another
void ValuesAndCalls(TestResults& results) {
    CheckProgramBoth(
        results, std::string(kBufs) +
                     "(const SQUARES (array u8 16) (make (array u8 16) 0 1 4 9 16 25 36 49 64\n"
                     "    81 100 121 144 169 196 225))\n"
                     "(let total u16 0)\n"
                     "(fn square ((v u8)) u8 (at SQUARES (& v 15)))\n"
                     "(fn mix ((p (ptr u8)) (k u8)) u8 (+ (square (load p)) k))\n"
                     "(fn sum () u16\n"
                     "    (with_aux true false\n"
                     "        (let s u16 0)\n"
                     "        (for i u8 0 255\n"
                     "            (set s (+ s (as u16 (load (addr_of (at book i)))))))\n"
                     "        s))\n"
                     "(fn main () void\n"
                     "    (set total (sum))\n"
                     "    (with_aux true true\n"
                     "        (for i u8 0 16\n"
                     "            (store (addr_of (at spare i)) (mix (addr_of (at book i)) i)))))");
}

// a region is a barrier: no load or store moves across its ends and no value passes them (a
// pass that did would read main memory's byte where aux memory's is meant, or the other way),
// and inside it a load reads what the switches pick, whatever the passes know of main memory
void Barriers(TestResults& results) {
    CheckProgramBoth(results,
                     std::string(kBufs) +
                         "(let mine (array u8 4) (make (array u8 4) 1 2 3 3))\n"
                         "(let flag u8 0)\n"
                         "(fn main () void\n"
                         "    (let p (ptr u8) (addr_of (at mine 1)))\n"
                         "    (let a u8 (load p))\n"
                         "    (let b u8 (with_aux true false (load p)))\n" // not a (cse)
                         "    (with_aux false true (store p 7))\n"         // to aux memory
                         "    (let c u8 (load p))\n"                       // main's, not 7
                         "    (let d u8 (with_aux true false (load p)))\n" // aux's 7
                         "    (store p 5)\n"
                         "    (let e u8 (with_aux true false (load p)))\n" // still 7, not 5
                         "    (for i u8 0 4\n"                             // not hoisted
                         "        (set (at out i) (with_aux true false (load (+ p 1)))))\n"
                         "    (let q (ptr u8) (addr_of (at mine 2)))\n"
                         "    (with_aux false true (store q 100))\n" // not (set mine 2)
                         "    (set (at out 10) (at mine 2))\n"       // main's 3, not 100
                         "    (let r (ptr u8) (addr_of (at mine 3)))\n"
                         // 200 at mine 3's address in aux memory, through a pointer the
                         // analyses do not follow to mine
                         "    (with_aux false true (store (as (ptr u8) (as u16 r)) 200))\n"
                         "    (let v u8 (with_aux true false (load r)))\n" // 200, though mine
                         "    (if (> v 3)\n"                               // only holds 1-3
                         "        (set flag 1))\n"
                         "    (set (at out 4) a)\n"
                         "    (set (at out 5) b)\n"
                         "    (set (at out 6) c)\n"
                         "    (set (at out 7) d)\n"
                         "    (set (at out 8) e)\n"
                         "    (set (at out 9) v))");
}

// lib/auxmem, and lib/dhgr's boxes, plot and text_row onto the screens, on the machine against
// the interpreter
void Libraries(TestResults& results) {
    CheckProgramBoth(results,
                     std::string("(import auxmem)\n") + kBufs +
                         "(fn main () void\n"
                         "    (auxmem.read (addr_of (at line 0)) (addr_of (at book 0)) 256)\n"
                         "    (auxmem.copy (addr_of (at spare 0)) (addr_of (at book 10)) 246)\n"
                         "    (set (at line 0) 99)\n"
                         "    (auxmem.write (addr_of (at book 255)) (addr_of (at line 0)) 1))");
    AuxRun r;
    std::string text = std::string("(import dhgr)\n") + kBufs +
                       "(fn main () void\n"
                       "    (dhgr.box dhgr.PAGE1 3 10 9 20 dhgr.ORANGE)\n"
                       "    (dhgr.fill dhgr.PAGE2 dhgr.AUX 4 100 7 5 0x11 0x22)\n"
                       "    (dhgr.masked dhgr.PAGE1 dhgr.AUX 4 12 4 6 (addr_of (at book 0))\n"
                       "        (addr_of (at book 100)))\n"
                       "    (dhgr.masked dhgr.PAGE1 dhgr.MAIN 4 12 4 6 (addr_of (at line 0))\n"
                       "        (addr_of (at line 30)))\n"
                       "    (dhgr.copy dhgr.PAGE2 dhgr.AUX 30 150 3 9 (addr_of (at book 7)))\n"
                       "    (dhgr.copy_box dhgr.PAGE2 dhgr.AUX 2 8 6 20 dhgr.PAGE1)\n"
                       "    (dhgr.copy_box dhgr.PAGE2 dhgr.MAIN 2 8 6 20 dhgr.PAGE1)\n"
                       "    (dhgr.plot dhgr.PAGE1 77 120 dhgr.PINK)\n"
                       "    (dhgr.plot dhgr.PAGE1 78 120 dhgr.AQUA)\n"
                       "    (set (at out 0) (dhgr.aux_rmw 0x2003 0x0F 0x40))\n"
                       "    (dhgr.text_row dhgr.PAGE1 22 (addr_of (at line 0))))";
    if (!RunBoth(results, text, r)) {
        return;
    }
    CheckSame(results, text, r);
    // both pages in both memories, and text row 22 (the rest of text page 1 starts as spaces
    // on the machine, zeros in the interpreter)
    bool same = true;
    for (std::uint32_t at = 0x0400; at < 0x6000 && same; at++) {
        bool screen = (at >= 0x0750 && at < 0x0778) || at >= 0x2000;
        same = !screen ||
               (r.machine->cpu.mem[at] == r.in.memory[at] && r.machine->aux[at] == r.in.aux[at]);
        if (!same) {
            std::fprintf(stderr, "dhgr: $%04X differs: machine %02X %02X, interpreter %02X %02X\n",
                         at, r.machine->cpu.mem[at], r.machine->aux[at], r.in.memory[at],
                         r.in.aux[at]);
        }
    }
    CHECK(results, same);
}

// the code that may run with RAMRD on, and the const data it reads, are in aux memory at the
// same addresses
void Mirrored(TestResults& results) {
    AuxRun r;
    std::string text = std::string(kBufs) +
                       "(const T (array u8 8) (make (array u8 8) 3 1 4 1 5 9 2 6))\n"
                       "(fn pick ((i u8)) u8 (with_aux true false (+ (at T (& i 7))\n"
                       "    (load (addr_of (at book i))))))\n"
                       "(fn main () void (set (at out 0) (pick 3)))";
    if (!RunBoth(results, text, r)) {
        return;
    }
    CheckSame(results, text, r);
    std::int32_t pick = tisple::FindFunction(r.program, "t.pick");
    std::uint16_t at = r.code.function_address[static_cast<std::size_t>(pick)];
    bool same = at != 0;
    for (std::uint16_t k = 0; k < 16 && same; k++) {
        same = r.machine->cpu.mem[at + k] == r.machine->aux[at + k];
    }
    CHECK(results, same);
    std::int32_t t = tisple::FindVar(r.program, "t.T");
    const tisple::VarPlace& place = r.code.layout.vars[static_cast<std::size_t>(t)];
    CHECK(results,
          !r.code.layout.mirrored.empty() && r.code.layout.mirrored[static_cast<std::size_t>(t)]);
    for (std::uint32_t k = 0; k < place.size; k++) {
        same = same && r.machine->aux[place.address + k] == r.machine->cpu.mem[place.address + k];
    }
    CHECK(results, same);
}

void ZeroPageFull(TestResults& results) {
    // 120 u16 locals in a function a with_aux calls: they all go in zero page, which they fill
    std::string text = "(fn big ((p (ptr u8))) u16\n";
    std::string sum = "v0";
    for (int i = 0; i < 120; i++) {
        text += "    (let v" + std::to_string(i) + " u16 (as u16 (load (+ p " + std::to_string(i) +
                "))))\n";
        if (i > 0) {
            sum = "(+ " + sum + " v" + std::to_string(i) + ")";
        }
    }
    text += "    " + sum + ")\n";
    text += "(let r u16 0)\n(fn main () void (set r (with_aux true false (big (as (ptr u8) "
            "0x2000)))))";
    ExpectLinkError(results, text, "do not fit in zero page");
}

void GuardWarning(TestResults& results) {
    std::string text = "(let ticks u8 0)\n"
                       "(fn tick () void :interrupt (set ticks (+ ticks 1)))\n"
                       "(fn main () void (with_aux false true (store (as (ptr u8) 0x2000) 1)))";
    Compiled c = CompileMainText(text);
    CHECK(results, c.ok);
    bool warned = false;
    for (const tisple::Diagnostic& d : c.code.warnings) {
        warned = warned || d.message.find("auxmem.guard") != std::string::npos;
    }
    CHECK(results, warned);
}

// Catwiz's content tables: 30 items with two strings each, printed a character at a time
// through a helper that reads inside a with_aux. The table :in aux (read by name in regions)
// and :strings aux (the table in main memory) against the same program all in main memory:
// the same text comes out, and the strings' bytes leave main memory
std::string ItemsProgram(const std::string& option) {
    bool aux = !option.empty();
    bool by_region = option == " :in aux";
    std::string text = "(struct Item ((name (ptr u8)) (text (ptr u8)) (cost u8)))\n"
                       "(const ITEMS (array Item 30) (make (array Item 30)\n";
    for (int k = 0; k < 30; k++) {
        std::string n = std::to_string(k);
        text += "    (make Item :name \"ITEM " + n + "\" :text \"" + n + ", AND IT IS TANGLED " +
                std::to_string(k * 7 % 30) + "\" :cost " + std::to_string(k % 7) + ")\n";
    }
    text += ")" + option + ")\n(let out (array u8 1500))\n(let n u16 0)\n(let total u8 0)\n";
    text += aux ? "(fn char_at ((s (ptr u8)) (i u8)) u8 (with_aux true false (load (+ s i))))\n"
                : "(fn char_at ((s (ptr u8)) (i u8)) u8 (load (+ s i)))\n";
    auto read = [&](const std::string& field) {
        std::string place = "(at ITEMS k " + field + ")";
        return by_region ? "(with_aux true false " + place + ")" : place;
    };
    text += "(fn put_str ((s (ptr u8))) void\n"
            "    (let i u8 0)\n"
            "    (while true\n"
            "        (let c u8 (char_at s i))\n"
            "        (if (= c 0) (break))\n"
            "        (set (at out n) c)\n"
            "        (set n (+ n 1))\n"
            "        (set i (+ i 1))))\n"
            "(fn name_of ((k u8)) (ptr u8) " +
            read("name") + ")\n(fn text_of ((k u8)) (ptr u8) " + read("text") +
            ")\n(fn cost_at ((k u8)) u8 (at ITEMS k cost))\n" +
            (by_region ? "(fn cost_of ((k u8)) u8 (with_aux true false (cost_at k)))\n"
                       : "(fn cost_of ((k u8)) u8 (cost_at k))\n") +
            "(fn main () void\n"
            "    (for k u8 0 30\n"
            "        (put_str (name_of k))\n"
            "        (put_str (text_of k))\n"
            "        (set total (+ total (cost_of k))))\n"
            "    (set total (+ total (at ITEMS 3 cost))))\n";
    return text;
}

// bytes of main memory (and the card) the build takes
std::size_t MainBytes(const tisple::Compiled& code) {
    std::size_t bytes = code.image.size();
    for (const tisple::MemorySegment& s : code.segments) {
        bytes += s.bank == tisple::MemoryBank::kAux ? 0 : s.bytes.size();
    }
    return bytes;
}

bool HasText(const std::vector<std::uint8_t>& bytes, const std::string& text) {
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
}

void AuxConstTables(TestResults& results) {
    std::string expected;
    for (int k = 0; k < 30; k++) {
        expected += "ITEM " + std::to_string(k) + std::to_string(k) + ", AND IT IS TANGLED " +
                    std::to_string(k * 7 % 30);
    }
    std::size_t main_bytes = 0;
    for (const std::string option : {"", " :in aux", " :strings aux"}) {
        for (int level : {1, 0}) {
            tisple::BuildOptions options;
            options.opt.level = level;
            AuxRun r;
            std::string text = ItemsProgram(option);
            if (!RunBoth(results, text, r, options)) {
                return;
            }
            CheckSame(results, text, r);
            std::int32_t out = tisple::FindVar(r.program, "t.out");
            std::uint16_t at = r.code.layout.vars[static_cast<std::size_t>(out)].address;
            std::string got(r.machine->cpu.mem + at, r.machine->cpu.mem + at + expected.size());
            CHECK(results, got == expected);
            std::size_t bytes = MainBytes(r.code);
            main_bytes = option.empty() ? bytes : main_bytes;
            bool in_main = HasText(r.code.image, "TANGLED");
            for (const tisple::MemorySegment& s : r.code.segments) {
                in_main =
                    in_main || (s.bank != tisple::MemoryBank::kAux && HasText(s.bytes, "TANGLED"));
            }
            // about 1.2 KB of text leaves main memory
            CHECK(results, option.empty() ? in_main : !in_main && bytes + 900 < main_bytes);
            if (level == 1) {
                std::printf("aux const tables%s: %zu bytes of main memory\n",
                            option.empty() ? "" : option.c_str(), bytes);
            }
        }
    }
}

} // namespace

void RunBackendAuxTests(TestResults& results) {
    ConstantBanks(results);
    RunTimeBanks(results);
    Exits(results);
    ValuesAndCalls(results);
    Barriers(results);
    Libraries(results);
    Mirrored(results);
    ZeroPageFull(results);
    GuardWarning(results);
    AuxConstTables(results);
}

} // namespace tisple_tests
