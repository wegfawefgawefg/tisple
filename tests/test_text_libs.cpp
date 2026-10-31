// lib/text80, lib/text40 and lib/auxmem on the built-in machine: what lands in which memory,
// and what the screen shows, text from print and from put_str alike

#include "machine/font.hpp"
#include "test_backend.hpp"
#include "test_machine.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

// a program with the real lib/, compiled and loaded, run until it returns; the program and
// its build in program and code when given
std::unique_ptr<tisple::Apple2> RunWithLib(TestResults& results, const std::string& text,
                                           tisple::Program* program_out = nullptr,
                                           tisple::Compiled* code_out = nullptr) {
    tisple::LoadOptions load;
    load.files.push_back(tisple::VirtualFile{"t.tsp", text});
    tisple::Program local_program;
    tisple::Compiled local_code;
    tisple::Program& program = program_out != nullptr ? *program_out : local_program;
    tisple::Compiled& code = code_out != nullptr ? *code_out : local_code;
    std::vector<tisple::Diagnostic> errors;
    bool ok = tisple::BuildProgram("t.tsp", load, program, errors) &&
              tisple::CompileProgram(program, tisple::BuildOptions(), code, errors);
    CHECK(results, ok);
    if (!ok) {
        std::fprintf(stderr, "%s", tisple::FormatDiagnostics(program.sources, errors).c_str());
        return nullptr;
    }
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    std::string error;
    for (const tisple::MemorySegment& s : code.segments) {
        CHECK(results, tisple::LoadSegment(*a, s, error));
    }
    CHECK(results,
          tisple::LoadProgram(*a, code.image.data(), code.image.size(), code.origin, error));
    tisple::RunApple2(*a, 20 * 1000 * 1000);
    CHECK(results, a->stop_reason == tisple::MachineStop::kReturned);
    return a;
}

// text row r's byte for column c of an 80-column screen
uint8_t Cell80(const tisple::Apple2& a, int c, int r) {
    int base = 0x0400 + (r % 8) * 0x80 + (r / 8) * 40;
    return (c % 2) == 0 ? a.aux[base + c / 2] : a.cpu.mem[base + c / 2];
}

uint8_t Cell40(const tisple::Apple2& a, int c, int r) {
    return a.cpu.mem[0x0400 + (r % 8) * 0x80 + (r / 8) * 40 + c];
}

void TestText80(TestResults& results) {
    auto a = RunWithLib(results, "(import text80)\n"
                                 "(fn main () void\n"
                                 "    (text80.on)\n"
                                 "    (text80.print \"Hi!\")\n"
                                 "    (text80.inverse true)\n"
                                 "    (text80.print \"Az\")\n"
                                 "    (text80.inverse false)\n"
                                 "    (text80.put_code 0x40)\n"
                                 "    (text80.move 79 1)\n"
                                 "    (text80.put_str \"qr\")\n"
                                 "    (text80.put 13)\n"
                                 "    (text80.put_str \"\")\n"
                                 "    (text80.put_str \"next\"))");
    if (a == nullptr) {
        return;
    }
    CHECK(results, a->sw.col80 && a->sw.altchar && a->sw.store80 && !a->sw.page2);
    CHECK(results, Cell80(*a, 0, 0) == 0xC8 && Cell80(*a, 1, 0) == 0xE9); // H i, in aux, main
    CHECK(results, Cell80(*a, 2, 0) == 0xA1);                             // !
    CHECK(results, Cell80(*a, 3, 0) == 0x01 && Cell80(*a, 4, 0) == 0x7A); // inverse A z
    CHECK(results, Cell80(*a, 5, 0) == 0x40);                             // MouseText as is
    CHECK(results, Cell80(*a, 6, 0) == 0xA0);                             // cleared
    // column 79 then a wrap; RETURN starts a line
    CHECK(results, Cell80(*a, 79, 1) == 0xF1 && Cell80(*a, 0, 2) == 0xF2);
    CHECK(results, Cell80(*a, 0, 3) == 0xEE && Cell80(*a, 3, 3) == 0xF4);
    // and the picture: an 80-column H at x 0, i at x 7, one pixel a dot
    tisple::Framebuffer fb;
    tisple::RenderScreen(*a, tisple::VideoLook::kMono, fb);
    bool drawn = true;
    for (int row = 0; row < 8; row++) {
        for (int dot = 0; dot < 7; dot++) {
            bool h = ((tisple::GlyphRow('H', row) >> dot) & 1) != 0;
            bool i = ((tisple::GlyphRow('i', row) >> dot) & 1) != 0;
            drawn = drawn && ((fb.pixels[row * tisple::kScreenWidth + dot] & 0xFFFFFF) != 0) == h;
            drawn =
                drawn && ((fb.pixels[row * tisple::kScreenWidth + 7 + dot] & 0xFFFFFF) != 0) == i;
        }
    }
    CHECK(results, drawn);

    // past the last row everything moves up a line, both halves
    auto b = RunWithLib(results, "(import text80)\n"
                                 "(fn main () void\n"
                                 "    (text80.on)\n"
                                 "    (text80.move 0 22)\n"
                                 "    (text80.print \"ab\")\n"
                                 "    (text80.move 78 23)\n"
                                 "    (text80.print \"xyz\"))");
    if (b == nullptr) {
        return;
    }
    CHECK(results, Cell80(*b, 0, 21) == 0xE1 && Cell80(*b, 1, 21) == 0xE2);
    CHECK(results, Cell80(*b, 78, 22) == 0xF8 && Cell80(*b, 79, 22) == 0xF9);
    CHECK(results, Cell80(*b, 0, 23) == 0xFA && Cell80(*b, 1, 23) == 0xA0);
}

void TestText40(TestResults& results) {
    auto a = RunWithLib(results, "(import text40)\n"
                                 "(fn main () void\n"
                                 "    (text40.on)\n"
                                 "    (text40.move 0 1)\n"
                                 "    (text40.print \"OK\")\n"
                                 "    (text40.inverse true)\n"
                                 "    (text40.put_str \"GO!\")\n"
                                 "    (text40.move 39 23)\n"
                                 "    (text40.inverse false)\n"
                                 "    (text40.put_str \"st\"))");
    if (a == nullptr) {
        return;
    }
    // the wrap off the last row scrolled everything up: OK was on row 1, s on row 23
    CHECK(results, Cell40(*a, 0, 0) == 0xCF && Cell40(*a, 1, 0) == 0xCB);
    CHECK(results, Cell40(*a, 2, 0) == 0x07 && Cell40(*a, 4, 0) == 0x21); // inverse G, !
    CHECK(results, Cell40(*a, 39, 22) == 0xF3 && Cell40(*a, 0, 23) == 0xF4);
    CHECK(results, !a->sw.col80);
}

void TestAuxMem(TestResults& results) {
    tisple::Program program;
    tisple::Compiled code;
    auto a = RunWithLib(results,
                        "(import auxmem)\n"
                        "(macro counting (n)\n"
                        "    `(make (array u8 ,n) ,@(map (fn (i) (% i 251)) (range 0 n))))\n"
                        "(let doc (array u8 700) (counting 700) :in aux)\n"
                        "(let buf (array u8 700))\n"
                        "(let out u16 0 :address 0x03C0)\n"
                        "(fn main () void\n"
                        "    (auxmem.read (addr_of (at buf 0)) (addr_of (at doc 0)) 700)\n"
                        "    (set (at buf 0) 77)\n"
                        "    (auxmem.write (addr_of (at doc 600)) (addr_of (at buf 0)) 1)\n"
                        "    (auxmem.read (addr_of (at buf 5)) (addr_of (at doc 600)) 1)\n"
                        "    (set out (+ (as u16 (at buf 699)) (as u16 (at buf 5)))))",
                        &program, &code);
    if (a == nullptr) {
        return;
    }
    // buf[699] is 699 % 251 = 197, buf[5] the 77 written to aux and read back
    CHECK(results, (a->cpu.mem[0x03C0] | (a->cpu.mem[0x03C1] << 8)) == 197 + 77);
    // read runs with RAMRD on, so its code is in both memories at the same address, and it
    // starts by turning RAMRD on (STA $C003); all switches off again
    std::uint16_t read = code.function_address[static_cast<std::size_t>(
        tisple::FindFunction(program, "auxmem.read"))];
    CHECK(results, read != 0 && a->cpu.mem[read] == 0x8D && a->aux[read] == 0x8D);
    CHECK(results, a->cpu.mem[read + 1] == 0x03 && a->aux[read + 1] == 0x03);
    CHECK(results, !a->sw.ramrd && !a->sw.ramwrt);
}

} // namespace

// aux to aux: 600 bytes (two pages and more) into another let, and 300 down a byte inside one
// (a forward copy, so the overlap is fine); the switches off after
void TestAuxCopy(TestResults& results) {
    tisple::Program program;
    tisple::Compiled code;
    auto a = RunWithLib(results,
                        "(import auxmem)\n"
                        "(macro counting (n)\n"
                        "    `(make (array u8 ,n) ,@(map (fn (i) (% i 251)) (range 0 n))))\n"
                        "(let doc (array u8 700) (counting 700) :in aux)\n"
                        "(let copy (array u8 600) :in aux)\n"
                        "(fn main () void\n"
                        "    (auxmem.copy (addr_of (at copy 0)) (addr_of (at doc 50)) 600)\n"
                        "    (auxmem.copy (addr_of (at doc 0)) (addr_of (at doc 1)) 300))",
                        &program, &code);
    if (a == nullptr) {
        return;
    }
    // where the two lets went
    auto address = [&](const char* name) {
        std::int32_t v = tisple::FindVar(program, name);
        return v < 0 ? std::uint16_t(0) : code.layout.vars[static_cast<std::size_t>(v)].address;
    };
    uint16_t doc = address("t.doc");
    uint16_t copy = address("t.copy");
    CHECK(results, doc >= 0x0800 && copy >= 0x0800);
    bool ok = true;
    for (int i = 0; i < 600; i++) {
        ok = ok && a->aux[copy + i] == (50 + i) % 251;
    }
    for (int i = 0; i < 300; i++) {
        ok = ok && a->aux[doc + i] == (i + 1) % 251; // down a byte: a forward copy is right
    }
    CHECK(results, ok);
    CHECK(results, a->aux[doc + 300] == 300 % 251);
    CHECK(results, !a->sw.ramrd && !a->sw.ramwrt);
}

void RunTextLibTests(TestResults& results) {
    TestText80(results);
    TestText40(results);
    TestAuxMem(results);
    TestAuxCopy(results);
}

} // namespace tisple_tests
