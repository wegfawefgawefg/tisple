// the memory map past one file: data and whole functions spill below the origin, around
// :reserve and :address lets; the language card when nothing needs the rom; lets :in aux in
// auxiliary memory; and the startup that switches the card in

#include "machine/apple2.hpp"
#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

using tisple::MemoryBank;

Compiled CompileWith(const std::string& text, const tisple::BuildOptions& options) {
    Compiled c;
    c.built = BuildText(text);
    if (!c.built.ok) {
        c.errors = ErrorText(c.built);
        return c;
    }
    std::vector<tisple::Diagnostic> errors;
    c.ok = tisple::CompileProgram(c.built.program, options, c.code, errors);
    if (!c.ok) {
        c.errors = tisple::FormatDiagnostics(c.built.program.sources, errors);
    }
    return c;
}

Compiled CompileAll(const std::string& text) {
    tisple::BuildOptions options;
    options.keep_all_functions = true;
    return CompileWith(text, options);
}

std::uint16_t AddressOf(const Compiled& c, const char* name) {
    std::int32_t var = tisple::FindVar(c.built.program, name);
    return var < 0 ? 0 : c.code.layout.vars[static_cast<std::size_t>(var)].address;
}

std::uint16_t FunctionAt(const Compiled& c, const char* name) {
    std::int32_t f = tisple::FindFunction(c.built.program, name);
    return f < 0 ? 0 : c.code.function_address[static_cast<std::size_t>(f)];
}

const tisple::MemorySegment* SegmentWith(const Compiled& c, MemoryBank bank,
                                         std::uint16_t address) {
    for (const tisple::MemorySegment& s : c.code.segments) {
        if (s.bank == bank && address >= s.address && address - s.address < s.bytes.size()) {
            return &s;
        }
    }
    return nullptr;
}

bool Report(const Compiled& c) {
    if (!c.ok) {
        std::fprintf(stderr, "%s\n", c.errors.c_str());
    }
    return c.ok;
}

const char* kSpill = "(let a (array u8 13000))\n"
                     "(let b (array u8 7000))\n"
                     "(fn f () u16\n"
                     "    (set (at a 12999) 5)\n"
                     "    (set (at b 6999) 7)\n"
                     "    (+ (as u16 (at a 12999)) (as u16 (at b 6999))))";

// data that does not fit after the code goes below the origin, as a segment of its own
void TestDataSpill(TestResults& results) {
    Compiled c = CompileAll(kSpill);
    if (!Report(c)) {
        CHECK(results, false);
        return;
    }
    std::uint16_t b = AddressOf(c, "t.b");
    CHECK(results, AddressOf(c, "t.a") >= 0x6000 && b >= 0x0800 && b + 7000 <= 0x6000);
    CHECK(results, SegmentWith(c, MemoryBank::kMain, b) != nullptr);
    CHECK(results, c.code.origin + c.code.image.size() <= 0x9500);
    CHECK(results, !c.code.card_ram);
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.f", {});

    // a :reserve page keeps it out: 7000 bytes do not fit $0800-$1FFF, so b goes above
    Compiled kept =
        CompileAll(std::string("(let page (array u8 8192) :address 0x2000 :reserve)\n") + kSpill);
    std::uint16_t moved = AddressOf(kept, "t.b");
    CHECK(results, kept.ok && moved >= 0x4000 && moved < 0x6000);
    // nothing loads over the reserved page
    for (const tisple::MemorySegment& s : kept.code.segments) {
        CHECK(results, s.address >= 0x4000 || s.address + s.bytes.size() <= 0x2000);
    }
}

// a macro that makes a function of n stores: 5 bytes of code each
const char* kBigFunctions =
    "(let buf (array u8 1000))\n"
    "(macro stores (n)\n"
    "    (let out (list))\n"
    "    (for i 0 n (set out (append out (list `(set (at buf ,i) ,(% (* i 7) 251))))))\n"
    "    `(do ,@out))\n"
    "(fn f1 () void (stores 1000))\n"
    "(fn f2 () void (stores 1000))\n"
    "(fn f3 () void (stores 1000))\n"
    "(fn f4 () void (stores 1000))\n"
    "(fn f5 () void (stores 1000))\n"
    "(fn f6 () u8 (f1) (f2) (f3) (f4) (f5) (at buf 999))\n";

// whole functions move below the origin when the code does not fit above it
void TestCodeSpill(TestResults& results) {
    Compiled c = CompileAll(kBigFunctions);
    if (!Report(c)) {
        CHECK(results, false);
        return;
    }
    int below = 0;
    for (const char* name : {"t.f1", "t.f2", "t.f3", "t.f4", "t.f5", "t.f6"}) {
        below += FunctionAt(c, name) < 0x6000 ? 1 : 0;
    }
    CHECK(results, below >= 2 && FunctionAt(c, "t.f1") >= 0x6000);
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.f6", {});
    // the listing reads every function's bytes, wherever they are
    std::string listing = tisple::ListProgram(c.built.program, c.code);
    std::uint16_t last = static_cast<std::uint16_t>(AddressOf(c, "t.buf") + 999);
    char sta[16];
    std::snprintf(sta, sizeof(sta), "8D %02X %02X", last & 0xFF, last >> 8);
    CHECK(results, listing.find(sta) != std::string::npos); // STA buf+999, in f5
}

const char* kCard = "(let a (array u8 13000))\n"
                    "(let b (array u8 20000))\n"
                    "(macro counting (n)\n"
                    "    `(make (array u8 ,n) ,@(map (fn (i) (% i 251)) (range 0 n))))\n"
                    "(let c (array u8 11000) (counting 11000))\n"
                    "(let result u16 0 :address 0x0300)\n"
                    "(fn main () void\n"
                    "    (set (at c 10999) 9)\n"
                    "    (set result (+ (as u16 (at c 99)) (as u16 (at c 10999)))))";

// past main memory: the language card, switched in by the startup
void TestLanguageCard(TestResults& results) {
    Compiled c = CompileAll(kCard);
    if (!Report(c)) {
        CHECK(results, false);
        return;
    }
    std::uint16_t at = AddressOf(c, "t.c");
    CHECK(results, c.code.card_ram && at >= 0xD000);
    const tisple::MemorySegment* card = SegmentWith(c, MemoryBank::kCard, at);
    CHECK(results, card != nullptr && card->bytes[at - card->address + 99] == 99);
    // the startup opens with CLD, BIT $C083, BIT $C083
    CHECK(results, c.code.image.size() > 7 && c.code.image[1] == 0x2C && c.code.image[2] == 0x83 &&
                       c.code.image[3] == 0xC0 && c.code.image[4] == 0x2C);
    std::string error;
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    for (const tisple::MemorySegment& s : c.code.segments) {
        CHECK(results, tisple::LoadSegment(*a, s, error));
    }
    CHECK(results,
          tisple::LoadProgram(*a, c.code.image.data(), c.code.image.size(), c.code.origin, error));
    tisple::RunApple2(*a, 2000000);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, a->cpu.mem[0x0300] == 108 && a->cpu.mem[0x0301] == 0);
    CHECK(results, !a->lc.read_ram); // the rom again after main

    // a jsr into the rom keeps the card out: then there is no room
    std::string calls_rom = std::string(kCard) + "\n(fn bell () void (jsr 0xFBDD))";
    Compiled no_card = CompileAll(calls_rom);
    CHECK(results, !no_card.ok && no_card.errors.find("no room") != std::string::npos);
    tisple::BuildOptions off;
    off.use_card = false;
    off.keep_all_functions = true;
    CHECK(results, !CompileWith(kCard, off).ok);
}

// :in aux: its own address space and its own segment, nothing in the main file
void TestAuxPlacement(TestResults& results) {
    Compiled c = CompileAll("(let doc (array u8 3) (make (array u8 3) 7 8 9) :in aux)\n"
                            "(let big (array u8 25000) :in aux)\n"
                            "(let page (array u8 64) :address 0x5000 :in aux)\n"
                            "(let mine (array u8 64) :address 0x5000)\n"
                            "(fn f () u16 (as u16 (addr_of (at doc 2))))");
    if (!Report(c)) {
        CHECK(results, false);
        return;
    }
    std::uint16_t doc = AddressOf(c, "t.doc");
    std::uint16_t big = AddressOf(c, "t.big");
    CHECK(results, doc >= 0x0800 && big >= 0x5040 && big + 25000 <= 0xC000); // around page
    const tisple::MemorySegment* aux = SegmentWith(c, MemoryBank::kAux, doc);
    CHECK(results, aux != nullptr && aux->bytes[doc - aux->address + 2] == 9);
    CHECK(results, SegmentWith(c, MemoryBank::kAux, 0x5000) == nullptr); // no value: untouched
    CHECK(results, c.code.image.size() < 1000);
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "t.f", {});

    ExpectLinkError(results,
                    "(let a (array u8 64) :address 0x5000 :in aux)\n"
                    "(let b (array u8 64) :address 0x5020 :in aux)\n(fn f () void)",
                    "overlaps a");
    ExpectLinkError(results, "(let a (array u8 50000) :in aux)\n(fn f () void)",
                    "no room for a (50000 bytes) in aux memory $0800-$BFFF");
}

// :reserve overlaps like any :address let; code flows around it, but never the startup
void TestReserveOverlaps(TestResults& results) {
    ExpectLinkError(results,
                    "(let page (array u8 8192) :address 0x2000 :reserve)\n"
                    "(let x u8 :address 0x2100)\n(fn f () void (set x 1))",
                    "x ($2100-$2100) overlaps page ($2000-$3FFF); move one");
    ExpectLinkError(results, "(let page (array u8 256) :address 0x6002 :reserve)\n(fn f () void)",
                    "an :address let overlaps the program's startup");
    Compiled around =
        CompileAll(std::string("(let page (array u8 256) :address 0x6100 :reserve)\n") +
                   "(let buf (array u8 1000))\n"
                   "(macro stores (n)\n"
                   "    (let out (list))\n"
                   "    (for i 0 n (set out (append out (list `(set (at buf ,i) 1)))))\n"
                   "    `(do ,@out))\n"
                   "(fn f1 () void (stores 60))\n"
                   "(fn f2 () void (stores 60))\n"
                   "(fn f3 () void (stores 60))\n"
                   "(fn f4 () void (f1) (f2) (f3))");
    CHECK(results, around.ok);
    for (const char* name : {"t.f1", "t.f2", "t.f3", "t.f4"}) {
        std::uint16_t at = FunctionAt(around, name);
        CHECK(results, at < 0x6100 || at >= 0x6200);
    }
    Diff d;
    CHECK(results, StartDiff(d, around));
    CheckDiff(results, d, "t.f4", {});
}

} // namespace

// a build that fails with an error that has part in it
void ExpectLinkError(TestResults& results, const std::string& text, const std::string& part) {
    Compiled c = CompileAll(text);
    bool ok = !c.ok && c.errors.find(part) != std::string::npos;
    if (!ok) {
        std::fprintf(stderr, "program: %s\n  want an error with: %s\n  got: %s\n", text.c_str(),
                     part.c_str(), c.ok ? "(built)" : c.errors.c_str());
    }
    CHECK(results, ok);
}

void RunLinkRegionTests(TestResults& results) {
    TestDataSpill(results);
    TestCodeSpill(results);
    TestLanguageCard(results);
    TestAuxPlacement(results);
    TestReserveOverlaps(results);
}

} // namespace tisple_tests
