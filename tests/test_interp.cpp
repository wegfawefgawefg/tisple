#include "test_check.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

std::int32_t VarId(const Built& built, const std::string& name) {
    std::int32_t id = tisple::FindVar(built.program, name);
    if (id < 0) {
        std::fprintf(stderr, "no var %s\n", name.c_str());
    }
    return id;
}

// element i of a top-level array of scalars, through the layout
std::uint16_t ReadElement(const tisple::Interp& in, const Built& built, std::int32_t var,
                          std::uint32_t i) {
    const tisple::Var& v = built.program.vars[static_cast<std::size_t>(var)];
    tisple::Cursor cursor = tisple::ElementCursor(built.program, built.layout, var,
                                                  tisple::VarCursor(built.layout, var), v.type, i);
    std::uint32_t size =
        tisple::TypeOf(built.program, tisple::TypeOf(built.program, v.type).elem).size;
    return tisple::ReadScalar(in, cursor, size);
}

std::uint16_t ReadGlobal(const tisple::Interp& in, const Built& built, const std::string& name) {
    std::int32_t var = VarId(built, name);
    std::uint32_t size = tisple::TypeOf(built.program, built.program.vars[var].type).size;
    return tisple::ReadScalar(in, tisple::VarCursor(built.layout, var), size);
}

void FillTests(TestResults& results) {
    Built built = BuildFile("examples/fill.tsp");
    tisple::Interp in;
    StartInterp(in, built);
    for (std::uint32_t a = 0x1F00; a < 0x4100; a++) {
        in.memory[a] = 0xAA;
    }
    CallOk(results, in, "fill.clear_hgr", {});
    bool cleared = true;
    for (std::uint32_t a = 0x2000; a < 0x4000; a++) {
        cleared = cleared && in.memory[a] == 0;
    }
    CHECK(results, cleared);
    CHECK(results, in.memory[0x1FFF] == 0xAA && in.memory[0x4000] == 0xAA);
}

void MacroTests(TestResults& results) {
    Built built = BuildFile("examples/macros.tsp");
    tisple::Interp in;
    StartInterp(in, built);
    // swap, then 3 incs each: (3, 10) -> (13, 6)
    CHECK(results, CallOk(results, in, "macros.shuffle", {3, 10}) == 19);
    // u8 wraps: (250, 10) -> (13, 253), sum 266 -> 10
    CHECK(results, CallOk(results, in, "macros.shuffle", {250, 10}) == 10);
    // the caller's own tmp is not captured by swap's tmp#
    Built hygiene =
        BuildText("(macro swap (args a b) `(do (let tmp# ,a) (set ,a ,b) (set ,b tmp#)))\n"
                  "(fn f (returns u8) (let tmp u8 1) (let other u8 2) (swap tmp other) "
                  "(- (* tmp 10) other))");
    StartInterp(in, hygiene);
    CHECK(results, CallOk(results, in, "t.f", {}) == 19);
}

void SpriteTests(TestResults& results) {
    Built built = BuildFile("examples/sprites.tsp");
    tisple::Interp in;
    StartInterp(in, built);
    CHECK(results, CallOk(results, in, "sprites.spawn", {10, 20, 0xFF}) == 1);
    CHECK(results, CallOk(results, in, "sprites.spawn", {255, 7, 3}) == 1);
    CHECK(results, ReadGlobal(in, built, "sprites.sprite_count") == 2);
    std::int32_t sprites = VarId(built, "sprites.sprites");
    const tisple::VarPlace& place = built.layout.vars[static_cast<std::size_t>(sprites)];
    CHECK(results, place.split && place.plane_stride == 32);
    // one 32-byte table per field: x, y, dx, active
    std::uint16_t base = place.address;
    CHECK(results, in.memory[base + 0] == 10 && in.memory[base + 32 + 0] == 20);
    CHECK(results, in.memory[base + 64 + 0] == 0xFF && in.memory[base + 96 + 0] == 1);
    CHECK(results, in.memory[base + 1] == 255 && in.memory[base + 96 + 1] == 1);
    CallOk(results, in, "sprites.step_sprites", {});
    // x + (u8 dx): 10 + 255 wraps to 9, 255 + 3 wraps to 2
    CHECK(results, in.memory[base + 0] == 9 && in.memory[base + 1] == 2);
    CHECK(results, in.memory[base + 32 + 0] == 20);
    for (int i = 2; i < 32; i++) {
        CallOk(results, in, "sprites.spawn", {0, 0, 0});
    }
    CHECK(results, CallOk(results, in, "sprites.spawn", {1, 1, 1}) == 0);
    CHECK(results, ReadGlobal(in, built, "sprites.sprite_count") == 32);
}

void HgrTests(TestResults& results) {
    Built dot = BuildFile("examples/dot/main.tsp");
    Built plot = BuildFile("examples/hires_plot.tsp");
    tisple::Interp in;
    StartInterp(in, dot);
    CallOk(results, in, "gfx/hgr.init", {});
    tisple::Interp table;
    StartInterp(table, plot);
    std::int32_t row_lo = VarId(dot, "gfx/hgr.row_lo");
    std::int32_t row_hi = VarId(dot, "gfx/hgr.row_hi");
    std::int32_t row_addr = VarId(plot, "hires_plot.ROW_ADDR");
    bool same = true;
    for (std::uint32_t y = 0; y < 192; y++) {
        std::uint16_t want = ReadElement(table, plot, row_addr, y);
        std::uint16_t got = static_cast<std::uint16_t>(ReadElement(in, dot, row_lo, y) |
                                                       (ReadElement(in, dot, row_hi, y) << 8));
        if (want != got) {
            std::fprintf(stderr, "row %u: init gives $%04X, ROW_ADDR has $%04X\n", y, got, want);
            same = false;
        }
    }
    CHECK(results, same);
    std::int32_t col_byte = VarId(dot, "gfx/hgr.col_byte");
    std::int32_t col_bit = VarId(dot, "gfx/hgr.col_bit");
    CHECK(results,
          ReadElement(in, dot, col_byte, 279) == 39 && ReadElement(in, dot, col_bit, 279) == 0x40);
    CHECK(results, ReadElement(in, dot, col_byte, 7) == 1 && ReadElement(in, dot, col_bit, 7) == 1);
    // ROW_ADDR is stored as split low and high tables
    const tisple::VarPlace& rows = plot.layout.vars[static_cast<std::size_t>(row_addr)];
    // with :boundary 256 each table gets a page of its own
    CHECK(results, rows.split && rows.plane_stride == 256 && rows.address % 256 + 192 <= 256);
    CHECK(results,
          table.memory[rows.address + 8] == 0x80 && table.memory[rows.address + 256 + 8] == 0x20);

    // flip sets one pixel, and again clears it
    CallOk(results, in, "gfx/hgr.flip", {8, 1});
    CHECK(results, in.memory[0x2401] == 0x02);
    CallOk(results, in, "gfx/hgr.flip", {8, 1});
    CHECK(results, in.memory[0x2401] == 0x00);
    // plot divides by 7 to find the byte
    CallOk(results, table, "hires_plot.plot", {15, 9});
    CHECK(results, table.memory[0x2482] == 0x02);
}

struct Keyboard {
    std::uint8_t key = 0;
    bool waiting = false;
    int strobes = 0;
};

void KeyboardTests(TestResults& results) {
    Built built = BuildFile("examples/dot/main.tsp");
    tisple::Interp in;
    StartInterp(in, built);
    Keyboard keyboard;
    in.io.read = [&keyboard](std::uint16_t address) -> std::uint8_t {
        if (address == 0xC000) {
            return static_cast<std::uint8_t>(keyboard.key | (keyboard.waiting ? 0x80 : 0));
        }
        if (address == 0xC010) {
            keyboard.waiting = false;
            keyboard.strobes++;
        }
        return 0;
    };
    // Action: none left right up down
    CHECK(results, CallOk(results, in, "main.read_action", {}) == 0);
    const std::uint8_t keys[] = {0x08, 0x15, 0x0B, 0x0A, 'x'};
    const std::uint16_t actions[] = {1, 2, 3, 4, 0};
    for (int i = 0; i < 5; i++) {
        keyboard.key = keys[i];
        keyboard.waiting = true;
        CHECK(results, CallOk(results, in, "main.read_action", {}) == actions[i]);
        CHECK(results, !keyboard.waiting);
    }
    CHECK(results, keyboard.strobes == 5);
    CHECK(results, CallOk(results, in, "main.read_action", {}) == 0);
}

void StopTests(TestResults& results) {
    Built spin = BuildText("(fn main (while true))");
    tisple::Interp in;
    StartInterp(in, spin);
    in.max_steps = 1000;
    tisple::InterpResult result = tisple::CallFunction(in, spin.program.main, {});
    CHECK(results, result.kind == tisple::StopKind::StepLimit);
    CHECK(results, result.message == "step limit of 1000 reached");
    Built divide = BuildText("(fn f (args (a u8) (b u8)) (returns u8) (/ a b))");
    StartInterp(in, divide);
    result = tisple::CallFunction(in, tisple::FindFunction(divide.program, "t.f"), {7, 0});
    CHECK(results, result.kind == tisple::StopKind::Error && result.message == "division by zero");
    CHECK(results, tisple::FormatLoc(divide.program.sources, result.loc) == "t.tsp:1:41");
}

void IoTests(TestResults& results) {
    Built built =
        BuildText("(fn f (returns u16) (io_write u16 0xC0F0 0x1234) (io_read u16 0xC0F2))");
    tisple::Interp in;
    StartInterp(in, built);
    std::vector<std::pair<std::uint16_t, int>> trace;
    in.io.write = [&trace](std::uint16_t address, std::uint8_t value) {
        trace.push_back({address, value});
    };
    in.io.read = [&trace](std::uint16_t address) -> std::uint8_t {
        trace.push_back({address, -1});
        return address == 0xC0F2 ? 0xCD : 0xAB;
    };
    CHECK(results, CallOk(results, in, "t.f", {}) == 0xABCD);
    bool order = trace.size() == 4 && trace[0].first == 0xC0F0 && trace[0].second == 0x34 &&
                 trace[1].first == 0xC0F1 && trace[1].second == 0x12 && trace[2].first == 0xC0F2 &&
                 trace[3].first == 0xC0F3;
    CHECK(results, order);
    // io never touches memory
    CHECK(results, in.memory[0xC0F0] == 0);
}

void SemanticsTests(TestResults& results) {
    tisple::Interp in;
    Built loops = BuildText(
        "(fn every_byte (returns u16) (let n u16 0) (for i u8 0 256 (set n (+ n 1))) n)\n"
        "(fn every_i8 (returns u16) (let n u16 0) (for i i8 -128 128 (set n (+ n 1))) n)\n"
        "(fn empty (returns u16) (let n u16 0) (for i u8 5 3 (set n (+ n 1))) n)\n"
        "(fn exits (returns u8) (let n u8 0)\n"
        "    (for i u8 0 100 (if (= i 3) (continue)) (if (= i 6) (break)) (set n (+ n i))) n)\n"
        "(fn counter (returns u8) (let s u8 0) (for i u8 250 255 (set s i)) s)");
    CHECK(results, loops.ok);
    StartInterp(in, loops);
    CHECK(results, CallOk(results, in, "t.every_byte", {}) == 256);
    CHECK(results, CallOk(results, in, "t.every_i8", {}) == 256);
    CHECK(results, CallOk(results, in, "t.empty", {}) == 0);
    CHECK(results, CallOk(results, in, "t.exits", {}) == 0 + 1 + 2 + 4 + 5);
    CHECK(results, CallOk(results, in, "t.counter", {}) == 254);

    Built pointers = BuildText(
        "(struct S (a u8) (b u16) (c u8))\n"
        "(let arr (array S 4))\n"
        "(fn step (returns u16) (let p (addr_of (at arr 0))) (- (as u16 (+ p 3)) (as u16 p)))\n"
        "(fn wide (returns u16) (let p (as (ptr u16) 0x1000)) (as u16 (+ p 5)))\n"
        "(fn copy (returns u16) (store (addr_of (at arr 2)) (S :a 1 :b 0x2345 :c 3))\n"
        "    (let s (at arr 2)) (let q (addr_of (at arr 1))) (store q (load (addr_of (at arr "
        "2))))\n"
        "    (+ s.b (u16 (. (at arr 1) c))))\n"
        "(fn frame (returns u8) (let y u8 0) (let r y) (set y 9) r)");
    CHECK(results, pointers.ok);
    if (!pointers.ok) {
        std::fprintf(stderr, "%s", ErrorText(pointers).c_str());
        return;
    }
    StartInterp(in, pointers);
    CHECK(results, CallOk(results, in, "t.step", {}) == 12);
    CHECK(results, CallOk(results, in, "t.wide", {}) == 0x100A);
    CHECK(results, CallOk(results, in, "t.copy", {}) == 0x2348);
    // a let sets its variable on every run, though the static frame still holds the 9
    CHECK(results, CallOk(results, in, "t.frame", {}) == 0);
    CHECK(results, CallOk(results, in, "t.frame", {}) == 0);
    // packed because of addr_of: element 2 starts 8 bytes in, b low byte first
    std::int32_t arr = VarId(pointers, "t.arr");
    std::uint16_t base = pointers.layout.vars[static_cast<std::size_t>(arr)].address;
    CHECK(results, in.memory[base + 8] == 1 && in.memory[base + 9] == 0x45 &&
                       in.memory[base + 10] == 0x23 && in.memory[base + 11] == 3);

    Built shifts = BuildText("(fn lsr (args (x u8)) (returns u8) (>> x 1))\n"
                             "(fn asr (args (x i8)) (returns i8) (>> x 1))\n"
                             "(fn widen (args (x i8)) (returns u16) (u16 x))\n"
                             "(fn narrow (args (x u16)) (returns i8) (i8 x))");
    StartInterp(in, shifts);
    CHECK(results, CallOk(results, in, "t.lsr", {0x80}) == 0x40);
    CHECK(results, CallOk(results, in, "t.asr", {0x80}) == 0xC0);
    CHECK(results, CallOk(results, in, "t.widen", {0xFF}) == 0xFFFF);
    CHECK(results, CallOk(results, in, "t.narrow", {0x1280}) == 0x80);
}

} // namespace

void RunInterpTests(TestResults& results) {
    FillTests(results);
    MacroTests(results);
    SpriteTests(results);
    HgrTests(results);
    KeyboardTests(results);
    StopTests(results);
    IoTests(results);
    SemanticsTests(results);
}

} // namespace tisple_tests
