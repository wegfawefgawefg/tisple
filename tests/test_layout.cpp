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
                            "(fn f (set (at table 1) 0x1234) (set (at table 3) 0xABCD))");
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
                             "(fn f (returns u16) (set (at table 1) 0x1234) "
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
                              "(fn f (returns u16) (let p (addr_of (at table 0))) "
                              "(store (+ p 2) 0xBEEF) (at table 2))");
    StartInterp(in, through);
    CHECK(results, CallOk(results, in, "t.f", {}) == 0xBEEF);

    // a struct array is one table per byte of the struct
    Built sprites = BuildText("(struct S (x u8) (pos u16) (on bool))\n(let s (array S 10))\n"
                              "(fn f (set (at s 2) (S :x 7 :pos 0x0102 :on true)))");
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
                             "(fn f (let local (array u16 3)))");
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
    // 0x1500 bytes do not fit below hi-res page 1, so they go above it
    CHECK(results, PlaceOf(placed, "t.big").address >= 0x4000);
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
            local_placed = placed.layout.vars[i].address >= 0x0C00 && placed.layout.vars[i].split;
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
    ExpectError(results, "(let a (array u8 40000))",
                "t.tsp:1:6: no room for a (40000 bytes) in the data region $0C00-$BFFF");
}

} // namespace

void RunLayoutTests(TestResults& results) {
    RepresentationTests(results);
    PlacementTests(results);
}

} // namespace tisple_tests
