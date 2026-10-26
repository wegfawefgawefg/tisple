#include "test_check.hpp"

namespace tisple_tests {

namespace {

const std::vector<tisple::VirtualFile> kHgr = {
    {"gfx/hgr.tsp", "(fn plot (args (x u8) (y u8)))\n(macro twice (args f) `(do ,f ,f))\n"},
};

void LoadingTests(TestResults& results) {
    ExpectError(results, "(import nothere)",
                "t.tsp:1:1: cannot find module nothere: no nothere.tsp or lib0/nothere.tsp");
    ExpectError(results, "(import b)",
                "b.tsp:1:1: import cycle: t -> b -> t; move what both "
                "need into a module they both import",
                {{"b.tsp", "(import t)"}});
    ExpectError(results, "(import x/hgr)\n(import y/hgr)",
                "t.tsp:2:1: two imports use the prefix hgr; rename one: (import y/hgr hgr2)",
                {{"x/hgr.tsp", ""}, {"y/hgr.tsp", ""}});
    ExpectError(results, "(import gfx/hgr Gfx)",
                "t.tsp:1:1: import prefix Gfx must be a snake_case name that is not built in; "
                "write (import gfx/hgr gfx_mod)",
                kHgr);
    // lib/ is searched after the entry folder, and a renamed prefix is the only name
    Built lib = BuildText("(import screen s)\n(fn f (returns u8) s.ROWS)",
                          {{"lib0/screen.tsp", "(const ROWS u8 24)"}});
    CHECK(results, lib.ok);
    ExpectError(results, "(fn f (\n", "t.tsp:1:1: this '(' is never closed");
}

void NamingTests(TestResults& results) {
    ExpectError(results, "(struct sprite (x u8))",
                "t.tsp:1:9: struct sprite: type names are PascalCase; write Sprite");
    ExpectError(results, "(enum action_kind none)",
                "t.tsp:1:7: enum action_kind: type names are PascalCase; write ActionKind");
    ExpectError(results, "(const max u8 1)",
                "t.tsp:1:8: const max: constant names are SCREAMING_SNAKE_CASE; write MAX");
    ExpectError(results, "(fn readKey)",
                "t.tsp:1:5: fn readKey: names are snake_case; write read_key");
    ExpectError(results, "(fn f (let myX u8 0))",
                "t.tsp:1:12: local myX: names are snake_case; write my_x");
    ExpectError(results, "(let Count u8 0)",
                "t.tsp:1:6: let Count: names are snake_case; write count");
    ExpectError(results, "(struct P (X u8))", "t.tsp:1:12: field X: names are snake_case; write x");
    ExpectError(results, "(enum A First)",
                "t.tsp:1:9: member First: names are snake_case; write first");
    ExpectError(results, "(fn f (args (Arg u8)))",
                "t.tsp:1:14: parameter Arg: names are snake_case; write arg");
    ExpectError(results, "(fn f (let a__1 u8 0))",
                "t.tsp:1:12: local a__1: names are snake_case; write a_1");
    ExpectError(results, "(macro Inc (args p) `(set ,p 1))",
                "t.tsp:1:8: macro Inc: names are snake_case; write inc");
    // built-in names stay built in
    ExpectError(results, "(fn f (let at u8 0))",
                "t.tsp:1:12: at is a built-in name; rename this local");
    ExpectError(results, "(fn load)", "t.tsp:1:5: load is a built-in name; rename this fn");
    ExpectError(results, "(struct P (ptr u8))",
                "t.tsp:1:12: ptr is a built-in name; rename this field");
    ExpectError(results, "(fn f (for as u8 0 3))",
                "t.tsp:1:12: as is a built-in name; rename this loop variable");
    // a local named like an import would make hgr.x ambiguous
    ExpectError(results, "(import gfx/hgr)\n(fn f (let hgr u8 0))",
                "t.tsp:2:12: hgr is an imported module here, so hgr.x would be ambiguous; "
                "rename this local",
                kHgr);
    ExpectError(results, "(import gfx/hgr)\n(fn f (args (hgr u8)))",
                "t.tsp:2:14: hgr is an imported module here, so hgr.x would be ambiguous; "
                "rename this parameter",
                kHgr);
}

void ResolveTests(TestResults& results) {
    ExpectError(results, "(fn f (returns u8) y)", "t.tsp:1:20: unknown name y");
    ExpectError(results, "(import gfx/hgr)\n(fn f (plot 1 2))",
                "t.tsp:2:7: unknown function plot; did you mean hgr.plot?", kHgr);
    ExpectError(results, "(import gfx/hgr)\n(fn f (hgr.plott))", "t.tsp:2:7: hgr has no plott",
                kHgr);
    ExpectError(results, "(struct P (x u8) (y u8))\n(let p P)\n(fn f (returns u8) p.z)",
                "t.tsp:3:20: P has no field z; its fields are x y");
    ExpectError(results, "(enum A one two)\n(fn f (returns A) A.three)",
                "t.tsp:2:19: A has no three");
    ExpectError(results, "(fn f) (fn f)",
                "t.tsp:1:12: f is declared twice in this module (first at t.tsp:1:5); rename one");
    ExpectError(results, "(fn f (let x u8 0) (let x u8 1))",
                "t.tsp:1:25: x is already declared in this body (t.tsp:1:12); pick another name");
    ExpectError(results, "(fn f (args (x u8)) (let x u8 1))",
                "t.tsp:1:26: x is already declared in this body (t.tsp:1:14); pick another name");
    ExpectError(results, "(let x u9 0)",
                "t.tsp:1:8: unknown type u9; types are u8 i8 u16 i16 bool, (ptr t), (array t n), "
                "or a struct or enum name");
    ExpectError(results, "(fn f (returns u8) f)", "t.tsp:1:20: f is a function; call it: (f ...)");
    ExpectError(results, "(enum A one)\n(fn f (returns u8) (A))",
                "t.tsp:2:20: nothing converts to an enum; name a member, as in A.one, or match on "
                "the number");
    // (. name x) is written name.x, except in code a macro made
    ExpectError(results, "(struct P (x u8))\n(let p P)\n(fn f (returns u8) (. p x))",
                "t.tsp:3:20: (. p x) reads a field of a plain name; write p.x");
    Built from_macro = BuildText("(struct P (x u8))\n(let p (P :x 7))\n"
                                 "(macro get_x (args s) `(. ,s x))\n"
                                 "(fn f (returns u8) (get_x p))");
    CHECK(results, from_macro.ok);
    if (from_macro.ok) {
        tisple::Interp in;
        StartInterp(in, from_macro);
        CHECK(results, CallOk(results, in, "t.f", {}) == 7);
    }
    // a module's macros are visible through its prefix
    Built twice = BuildText("(import gfx/hgr)\n(fn f (hgr.twice (hgr.plot 1 2)))", kHgr);
    CHECK(results, twice.ok);
    ExpectError(results, "(fn f (returns u8) (do (let n u8 1)) n)", "t.tsp:1:38: unknown name n");
}

} // namespace

void RunCheckNameTests(TestResults& results) {
    LoadingTests(results);
    NamingTests(results);
    ResolveTests(results);
}

} // namespace tisple_tests
