#include "test_check.hpp"

#include <cstdio>

namespace tisple_tests {

// the owner's answers of 2026-10-26: shift counts, fields through pointers, constant
// patterns, enum to integer, macro name homes, local let values, const table placement

namespace {

std::uint16_t Run(TestResults& results, const std::string& text, const std::string& function,
                  const std::vector<std::uint16_t>& args = {},
                  const std::vector<tisple::VirtualFile>& extra = {}) {
    Built built = BuildText(text, extra);
    if (!built.ok) {
        std::fprintf(stderr, "%s", ErrorText(built).c_str());
    }
    CHECK(results, built.ok);
    if (!built.ok) {
        return 0;
    }
    tisple::Interp in;
    StartInterp(in, built);
    return CallOk(results, in, function, args);
}

void ShiftTests(TestResults& results) {
    ExpectError(results, "(fn f (args (x u16) (n u16)) (returns u16) (<< x n))",
                "t.tsp:1:50: the shift count needs a u8 but n is u16; write (u8 n)");
    ExpectError(results, "(fn f (args (n u8)) (let m (<< 1 n)))",
                "t.tsp:1:32: (<< 1 n): the shifted literal has no type; give the result one, as "
                "in (let m u8 (<< 1 n))");
    ExpectError(results, "(fn f (args (b bool) (n u8)) (returns u8) (>> b n))",
                "t.tsp:1:43: >> works on integers, not bool");
    // a u16 shifted by a u8 count, and a literal taking the result's type
    CHECK(results, Run(results, "(fn f (args (x u16) (n u8)) (returns u16) (<< x n))", "t.f",
                       {0x0123, 4}) == 0x1230);
    CHECK(results,
          Run(results, "(fn f (args (n u8)) (returns u16) (<< 1 n))", "t.f", {12}) == 0x1000);
}

void PointerFieldTests(TestResults& results) {
    const std::string types = "(struct Pos (x u8) (y u16))\n(struct Obj (id u8) (pos Pos))\n";
    std::string program = types +
                          "(let objs (array Obj 4))\n"
                          "(fn f (returns u16)\n"
                          "    (let p (addr_of (at objs 1)))\n"
                          "    (store p id 7)\n"
                          "    (store p pos y 0x1234)\n"
                          "    (store (+ p 1) pos (Pos :x 9 :y 0x0042))\n"
                          "    (+ (load p pos y) (u16 (+ (load p id) (load (+ p 1) pos x)))))";
    CHECK(results, Run(results, program, "t.f") == 0x1234 + 7 + 9);
    Built built = BuildText(program);
    // the field offsets are in the typed program for the backend: pos.y is 1 + 1 bytes in
    bool found_load = false;
    bool found_store = false;
    for (const tisple::Expr& expr : built.program.exprs) {
        found_load = found_load || (expr.kind == tisple::ExprKind::LoadField && expr.value == 2 &&
                                    expr.type == tisple::kU16Type);
        found_store = found_store || (expr.kind == tisple::ExprKind::StoreField && expr.value == 2);
    }
    CHECK(results, found_load && found_store);

    const std::string fn = "(fn f (args (p (ptr Obj))) (returns u8) ";
    ExpectError(results, types + fn + "(load p z))",
                "t.tsp:3:49: Obj has no field z; its fields are id pos");
    ExpectError(results, types + fn + "(load p id x))",
                "t.tsp:3:52: p points to u8, which has no field x");
    ExpectError(results, types + fn + "(. (load p) id))",
                "t.tsp:3:41: (. (load p) id) reads a field through a pointer; write (load p id)");
    ExpectError(results, types + "(fn f (args (p (ptr Obj))) (store p pos x 300))",
                "t.tsp:3:43: 300 does not fit in u8 (0 to 255)");
    ExpectError(results, types + "(fn f (args (p (ptr Obj))) (store p id))",
                "t.tsp:3:37: unknown name id");
}

void PatternTests(TestResults& results) {
    // named constants, also from another module, match like their values
    std::string keys = "(import apple2 a2)\n(const OTHER u8 0x41)\n"
                       "(fn f (args (k u8)) (returns u8)\n"
                       "    (match k (a2.KEY_ESC 1) (a2.KEY_LEFT 2) (OTHER 3) (_ 0)))";
    std::vector<tisple::VirtualFile> lib = {
        {"lib0/apple2.tsp", "(const KEY_ESC u8 0x1B)\n(const KEY_LEFT u8 0x08)"}};
    CHECK(results, Run(results, keys, "t.f", {0x1B}, lib) == 1);
    CHECK(results, Run(results, keys, "t.f", {0x41}, lib) == 3);
    CHECK(results, Run(results, keys, "t.f", {0x09}, lib) == 0);
    ExpectError(results,
                "(const K u8 8)\n(fn f (args (x u8)) (match x (K (do)) (8 (do)) (_ (do))))",
                "t.tsp:2:40: 8 is matched twice; remove one clause");
    ExpectError(results, "(fn f (args (x u8) (y u8)) (match x (y (do)) (_ (do))))",
                "t.tsp:1:38: y is not a constant; a pattern is a number, a constant, an enum "
                "member or _");
    ExpectError(results, "(const K u16 8)\n(fn f (args (x u8)) (match x (K (do)) (_ (do))))",
                "t.tsp:2:31: a pattern of this match needs a u8 but K is u16; write (u8 K)");
}

void EnumConversionTests(TestResults& results) {
    std::string program = "(enum Dir (north 3) east south)\n"
                          "(fn f (args (d Dir)) (returns u16) (+ (u16 d) (u16 (u8 Dir.south))))";
    CHECK(results, Run(results, program, "t.f", {4}) == 4 + 5);
    ExpectError(results, "(enum Dir north)\n(fn f (args (x u8)) (returns Dir) (Dir x))",
                "t.tsp:2:35: nothing converts to an enum; name a member, as in Dir.north, or "
                "match on the number");
    ExpectError(results, "(enum Dir north)\n(fn f (args (x u8)) (returns Dir) (as Dir x))",
                "t.tsp:2:35: as converts between u16 and pointers: (as (ptr u8) n) or (as u16 p); "
                "for integers write (u8 x)");
}

void MacroHomeTests(TestResults& results) {
    // the template's plot is gfx/hgr's even though main has its own plot; ,x is main's local
    std::vector<tisple::VirtualFile> hgr = {
        {"gfx/hgr.tsp", "(let last u8 0)\n"
                        "(fn plot (args (x u8)) (set last x))\n"
                        "(const SCALE u8 2)\n"
                        "(macro plot_scaled (args x) `(plot (* ,x SCALE)))\n"}};
    std::string main = "(import gfx/hgr)\n"
                       "(let mine u8 0)\n"
                       "(fn plot (args (x u8)) (set mine x))\n"
                       "(fn f (returns u16) (let x u8 5) (hgr.plot_scaled x) (+ (u16 hgr.last) "
                       "(* (u16 mine) 256)))";
    CHECK(results, Run(results, main, "t.f", {}, hgr) == 10);
    // a template name the macro's module lacks is an error there, naming the macro
    std::vector<tisple::VirtualFile> broken = {
        {"gfx/hgr.tsp", "(macro poke (args x) `(helper ,x))\n"}};
    ExpectError(results, "(import gfx/hgr)\n(fn helper (args (x u8)))\n(fn f (hgr.poke 1))",
                "t.tsp:3:7: unknown function helper (in macro hgr.poke)", broken);
}

void LocalLetTests(TestResults& results) {
    ExpectError(results, "(fn f (let x u8))",
                "t.tsp:1:7: a local let needs a value; write (let x u8 0)");
    ExpectError(results, "(fn f (let b bool))",
                "t.tsp:1:7: a local let needs a value; write (let b bool false)");
    ExpectError(results, "(struct P (x u8) (y u8))\n(fn f (let p P))",
                "t.tsp:2:7: a local let needs a value; write (let p (P :x ... :y ...))");
    ExpectError(results, "(fn f (let buf (array u8 8)))",
                "t.tsp:1:7: a local let needs a value; arrays have no value to start from, so "
                "make buf a top-level let");
    // a top-level let without a value is zero in the image
    Built zero = BuildText("(let counter u16)\n(let flags (array u8 4))");
    CHECK(results, zero.ok);
    tisple::Interp in;
    StartInterp(in, zero);
    std::int32_t counter = tisple::FindVar(zero.program, "t.counter");
    std::uint16_t at = zero.layout.vars[static_cast<std::size_t>(counter)].address;
    CHECK(results, in.memory[at] == 0 && in.memory[at + 1] == 0);
}

void PlacementTests(TestResults& results) {
    Built table = BuildText("(const T (array u16 3) 1 2 3 :align 256)\n"
                            "(let screen (array u8 8192) :address 0x2000)\n"
                            "(fn f (set (at screen 5) 0xFF))");
    CHECK(results, table.ok);
    if (!table.ok) {
        std::fprintf(stderr, "%s", ErrorText(table).c_str());
        return;
    }
    std::int32_t t = tisple::FindVar(table.program, "t.T");
    const tisple::VarPlace& place = table.layout.vars[static_cast<std::size_t>(t)];
    CHECK(results, place.address % 256 == 0 && place.split);
    // an explicit address may point at the hi-res screen
    tisple::Interp in;
    StartInterp(in, table);
    CallOk(results, in, "t.f", {});
    CHECK(results, in.memory[0x2005] == 0xFF);
    CHECK(results, in.memory[place.address + 2] == 3 && in.memory[place.address + 3 + 2] == 0);
    ExpectError(results, "(const K u8 1 :align 2)",
                "t.tsp:1:15: placement options are for const tables and top-level lets; K is a "
                "single value with no address");
    ExpectError(results, "(const T (array u8 2) :align 256 1 2)",
                "t.tsp:1:34: placement options come last, after every value");
    ExpectError(results, "(const T (array u8 300) :boundary 256)",
                "t.tsp:1:1: T is (array u8 300) but has 0 values; give exactly 300");
}

} // namespace

void RunCheckDecisionTests(TestResults& results) {
    ShiftTests(results);
    PointerFieldTests(results);
    PatternTests(results);
    EnumConversionTests(results);
    MacroHomeTests(results);
    LocalLetTests(results);
    PlacementTests(results);
}

} // namespace tisple_tests
