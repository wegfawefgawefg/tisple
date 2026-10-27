#include "test_check.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

const tisple::VarPlace& PlaceOf(const Built& built, const std::string& name) {
    static const tisple::VarPlace kNone;
    std::int32_t var = tisple::FindVar(built.program, name);
    if (var < 0) {
        std::fprintf(stderr, "no var %s\n", name.c_str());
        return kNone;
    }
    return built.layout.vars[static_cast<std::size_t>(var)];
}

bool InReserved(std::uint32_t start, std::uint32_t size) {
    for (const tisple::AddressRange& range : tisple::kReservedRanges) {
        if (start < range.end && range.start < start + size) {
            return true;
        }
    }
    return false;
}

void RepresentationTests(TestResults& results) {
    // a u16 array is a low-byte table followed by a high-byte table
    Built split = BuildText("(let table (array u16 4))\n"
                            "(fn f () void (set (at table 1) 0x1234) (set (at table 3) 0xABCD))");
    CHECK(results, split.ok);
    const tisple::VarPlace& table = PlaceOf(split, "t.table");
    CHECK(results, table.split && table.plane_stride == 4 && table.size == 8);
    tisple::Interp in;
    StartInterp(in, split);
    CallOk(results, in, "t.f", {});
    std::uint16_t base = table.address;
    CHECK(results, in.memory[base + 1] == 0x34 && in.memory[base + 4 + 1] == 0x12);
    CHECK(results, in.memory[base + 3] == 0xCD && in.memory[base + 4 + 3] == 0xAB);
    CHECK(results, in.memory[base + 2] == 0 && in.memory[base + 0] == 0);

    // an addr_of into it makes it packed, low byte first
    Built packed = BuildText("(let table (array u16 4))\n"
                             "(fn f () u16 (set (at table 1) 0x1234) "
                             "(as u16 (addr_of (at table 1))))");
    CHECK(results, packed.ok);
    const tisple::VarPlace& packed_table = PlaceOf(packed, "t.table");
    CHECK(results, !packed_table.split);
    StartInterp(in, packed);
    std::uint16_t pointer = CallOk(results, in, "t.f", {});
    CHECK(results, pointer == packed_table.address + 2);
    CHECK(results, in.memory[pointer] == 0x34 && in.memory[pointer + 1] == 0x12);

    // pointers to u16 arrays are the same bytes through load and store
    Built through = BuildText("(let table (array u16 4))\n"
                              "(fn f () u16 (let p (ptr u16) (addr_of (at table 0))) "
                              "(store (+ p 2) 0xBEEF) (at table 2))");
    StartInterp(in, through);
    CHECK(results, CallOk(results, in, "t.f", {}) == 0xBEEF);

    // a struct array is one table per byte of the struct
    Built sprites = BuildText("(struct S ((x u8) (pos u16) (on bool)))\n(let s (array S 10))\n"
                              "(fn f () void (set (at s 2) (make S :x 7 :pos 0x0102 :on true)))");
    const tisple::VarPlace& s = PlaceOf(sprites, "t.s");
    CHECK(results, s.split && s.plane_stride == 10 && s.size == 40);
    StartInterp(in, sprites);
    CallOk(results, in, "t.f", {});
    base = s.address;
    CHECK(results, in.memory[base + 2] == 7 && in.memory[base + 10 + 2] == 0x02);
    CHECK(results, in.memory[base + 20 + 2] == 0x01 && in.memory[base + 30 + 2] == 1);
}

void PlacementTests(TestResults& results) {
    Built placed = BuildText("(let screen (array u8 40) :address 0x0400)\n"
                             "(let page (array u8 256) :align 256)\n"
                             "(let rows (array u16 192) :boundary 256)\n"
                             "(let small u8 1)\n"
                             "(let big (array u8 0x1500))\n"
                             "(fn f () void (let local u16 3))");
    CHECK(results, placed.ok);
    if (!placed.ok) {
        std::fprintf(stderr, "%s", ErrorText(placed).c_str());
        return;
    }
    CHECK(results, PlaceOf(placed, "t.screen").address == 0x0400);
    CHECK(results, PlaceOf(placed, "t.page").address % 256 == 0);
    // a split u16 table with :boundary keeps each byte table inside one page
    const tisple::VarPlace& rows = PlaceOf(placed, "t.rows");
    CHECK(results, rows.split && rows.plane_stride == 256 && rows.address % 256 + 192 <= 256);
    // the hi-res pages are ordinary memory until the program keeps one with :reserve
    CHECK(results, PlaceOf(placed, "t.big").address < 0x2000 + 0x1500);
    for (std::size_t i = 0; i < placed.layout.vars.size(); i++) {
        const tisple::VarPlace& place = placed.layout.vars[i];
        bool fixed = placed.program.vars[i].placement.has_address;
        CHECK(results, fixed || !InReserved(place.address, place.size));
        CHECK(results, fixed || place.address >= 0x0C00);
    }
    // a local gets a frame slot too
    bool local_placed = false;
    for (std::size_t i = 0; i < placed.program.vars.size(); i++) {
        if (placed.program.vars[i].name == "local") {
            local_placed = placed.layout.vars[i].address >= 0x0C00;
        }
    }
    CHECK(results, local_placed);

    ExpectError(
        results, "(let a (array u8 300) :boundary 256)",
        "t.tsp:1:6: a is 300 bytes, more than its :boundary 256; drop :boundary or shrink it");
    ExpectError(results, "(let a (array u16 300) :boundary 256)",
                "t.tsp:1:6: a has 300-byte tables, more than its :boundary 256; shorten the array "
                "or drop :boundary");
    ExpectError(results, "(let a (array u8 16) :address 0x6000)\n(let b u8 :address 0x600F)",
                "t.tsp:2:6: b ($600F-$600F) overlaps a ($6000-$600F); move one");
    ExpectError(results, "(let a u8 :address 0x6001 :align 256)",
                "t.tsp:1:6: :address $6001 is not a multiple of :align 256; use $6100 or drop "
                ":align");
    ExpectError(results, "(let a (array u8 16) :address 0x60F8 :boundary 256)",
                "t.tsp:1:6: a at $60F8 crosses a 256-byte block; start it at $6100");
    ExpectError(results, "(let a (array u8 512) :address 0xFF00)",
                "t.tsp:1:6: a at $FF00 is 512 bytes and runs past $FFFF; move it lower");
    // pointer provenance: a constant address inside a declared variable must be addr_of
    ExpectError(results,
                "(let screen (array u8 8192) :address 0x2000)\n"
                "(fn f () void (store (as (ptr u8) (+ 0x2000 5)) 1))",
                "t.tsp:2:22: the constant address $2005 is inside screen ($2000-$3FFF); a pointer "
                "into a variable comes from addr_of, as in (addr_of screen)");
    ExpectError(results, "(let buf (array u8 16))\n(fn f () u8 (load (as (ptr u8) 0x0C05)))",
                "t.tsp:2:19: the constant address $0C05 is inside buf ($0C00-$0C0F); a pointer "
                "into a variable comes from addr_of, as in (addr_of buf)");
    CHECK(results, BuildText("(let buf (array u8 16))\n"
                             "(fn f () u8 (load (as (ptr u8) 0x2005)))")
                       .ok);
    ExpectError(results, "(let a (array u8 50000))",
                "t.tsp:1:6: no room for a (50000 bytes) in the data region $0C00-$BFFF");
}

// every byte table of this array inside one page
bool TablesInPages(const Built& built, const std::string& name, std::uint32_t count,
                   std::uint32_t tables) {
    const tisple::VarPlace& place = PlaceOf(built, name);
    std::uint32_t stride = place.split ? place.plane_stride : count;
    for (std::uint32_t j = 0; j < tables; j++) {
        if ((place.address + j * stride) % 256 + count > 256) {
            return false;
        }
    }
    return true;
}

// automatic placement keeps byte tables inside pages, so indexed reads never cross one,
// and fills the gaps it leaves with what comes later
void PageTableTests(TestResults& results) {
    Built built = BuildText("(struct Rgb ((r u8) (g u8) (b u8)))\n"
                            "(let first (array u8 7))\n"
                            "(let sine (array u8 256))\n"
                            "(let rows (array u16 192))\n"
                            "(let col (array u8 40))\n"
                            "(let row (array u8 48))\n"
                            "(let pairs (array i16 200))\n"
                            "(let colors (array Rgb 200))\n"
                            "(let wide (array u8 300))\n"
                            "(let mine (array u8 100) :align 2)\n"
                            "(fn f () u8 (at sine (at col (at row 3))))");
    CHECK(results, built.ok);
    if (!built.ok) {
        std::fprintf(stderr, "%s", ErrorText(built).c_str());
        return;
    }
    CHECK(results, PlaceOf(built, "t.sine").address % 256 == 0);
    CHECK(results, TablesInPages(built, "t.rows", 192, 2));
    CHECK(results, TablesInPages(built, "t.col", 40, 1));
    CHECK(results, TablesInPages(built, "t.row", 48, 1));
    CHECK(results, TablesInPages(built, "t.pairs", 200, 2));
    // col and row go in the gap before sine
    CHECK(results, PlaceOf(built, "t.col").address < PlaceOf(built, "t.sine").address);
    CHECK(results, PlaceOf(built, "t.row").address < PlaceOf(built, "t.sine").address);
    // :align is the author's choice
    CHECK(results, PlaceOf(built, "t.mine").address % 2 == 0);
    // three tables of 200, 200 apart, have no start that keeps all three in pages: placed
    // anyway, and nothing overlaps
    CHECK(results, PlaceOf(built, "t.colors").address >= 0x0C00);
    for (std::size_t i = 0; i < built.layout.vars.size(); i++) {
        for (std::size_t j = i + 1; j < built.layout.vars.size(); j++) {
            const tisple::VarPlace& a = built.layout.vars[i];
            const tisple::VarPlace& b = built.layout.vars[j];
            bool frame = built.program.vars[i].function >= 0 || built.program.vars[j].function >= 0;
            bool apart = a.address + a.size <= b.address || b.address + b.size <= a.address;
            CHECK(results, frame || a.size == 0 || b.size == 0 || apart);
        }
    }
}

} // namespace

void RunLayoutTests(TestResults& results) {
    RepresentationTests(results);
    PlacementTests(results);
    PageTableTests(results);
}

} // namespace tisple_tests
