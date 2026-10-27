// lib/text80, lib/text40 and lib/auxmem on the built-in machine: what lands in which memory,
// and what the screen shows

#include "machine/font.hpp"
#include "test_backend.hpp"
#include "test_machine.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

// a program with the real lib/, compiled and loaded, run until it returns
std::unique_ptr<tisple::Apple2> RunWithLib(TestResults& results, const std::string& text) {
    tisple::LoadOptions load;
    load.files.push_back(tisple::VirtualFile{"t.tsp", text});
    tisple::Program program;
    std::vector<tisple::Diagnostic> errors;
    tisple::Compiled code;
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
                                 "    (text80.print \"qr\")\n"
                                 "    (text80.put 13)\n"
                                 "    (text80.print \"next\"))");
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
                                 "    (text40.print \"GO!\")\n"
                                 "    (text40.move 39 23)\n"
                                 "    (text40.inverse false)\n"
                                 "    (text40.print \"st\"))");
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
    auto a =
        RunWithLib(results, "(import auxmem)\n"
                            "(macro counting (n)\n"
                            "    `(make (array u8 ,n) ,@(map (fn (i) (% i 251)) (range 0 n))))\n"
                            "(let doc (array u8 700) (counting 700) :in aux)\n"
                            "(let buf (array u8 700))\n"
                            "(let out u16 0 :address 0x0380)\n"
                            "(fn main () void\n"
                            "    (auxmem.read (addr_of (at buf 0)) (addr_of (at doc 0)) 700)\n"
                            "    (set (at buf 0) 77)\n"
                            "    (auxmem.write (addr_of (at doc 600)) (addr_of (at buf 0)) 1)\n"
                            "    (auxmem.read (addr_of (at buf 5)) (addr_of (at doc 600)) 1)\n"
                            "    (set out (+ (as u16 (at buf 699)) (as u16 (at buf 5)))))");
    if (a == nullptr) {
        return;
    }
    // buf[699] is 699 % 251 = 197, buf[5] the 77 written to aux and read back
    CHECK(results, (a->cpu.mem[0x0380] | (a->cpu.mem[0x0381] << 8)) == 197 + 77);
    // the mover in page 3 of both memories; all switches off again; zero page as it was
    CHECK(results, a->cpu.mem[0x0300] == 0x48 && a->aux[0x0300] == 0x48);
    CHECK(results, !a->sw.ramrd && !a->sw.ramwrt);
}

} // namespace

void RunTextLibTests(TestResults& results) {
    TestText80(results);
    TestText40(results);
    TestAuxMem(results);
}

} // namespace tisple_tests
