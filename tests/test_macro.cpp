// computing macros: the evaluator's values and forms, typed calls, limits and lib/tables

#include "check/expand.hpp"
#include "reader/format.hpp"
#include "test_backend.hpp"
#include "test_check.hpp"

#include <cmath>
#include <cstdio>
#include <numbers>

namespace tisple_tests {

namespace {

// the entry module after expansion, through the formatter; lib/ is the repo's
std::string Expanded(const std::string& text) {
    tisple::LoadOptions options;
    options.files.push_back(tisple::VirtualFile{"t.tsp", text});
    tisple::LoadedProgram loaded;
    std::vector<tisple::Diagnostic> errors;
    if (!tisple::LoadProgram("t.tsp", options, loaded, errors) ||
        !tisple::ExpandProgram(loaded, errors)) {
        return "error: " + tisple::FormatDiagnostics(loaded.sources, errors);
    }
    const tisple::Module& module = loaded.modules[0];
    return tisple::FormatTree(loaded.sources.files[module.file], module.tree, false);
}

void ExpectExpanded(TestResults& results, const std::string& text, const std::string& expected) {
    std::string got = Expanded(text);
    if (got != expected) {
        std::fprintf(stderr, "macro: %s\n  want: %s  got:  %s", text.c_str(), expected.c_str(),
                     got.c_str());
    }
    CHECK(results, got == expected);
}

// the value a macro body gives, pasted as the value of a const; decls go before the macro
void ExpectValue(TestResults& results, const std::string& body, const std::string& expected,
                 const std::string& decls = "") {
    std::string text = decls + "(macro v () " + body + ")\n(const X u8 (v))";
    std::string want = "(const X u8 " + expected + ")\n";
    std::string got = Expanded(text);
    std::size_t at = got.find("(const X");
    got = at == std::string::npos ? got : got.substr(at);
    if (got != want) {
        std::fprintf(stderr, "macro value: %s\n  want: %s  got:  %s", body.c_str(), want.c_str(),
                     got.c_str());
    }
    CHECK(results, got == want);
}

// the first error of a program that does not expand, with lib/ the repo's
void ExpectMacroError(TestResults& results, const std::string& text, const std::string& expected) {
    std::string got = Expanded(text);
    std::string want = "error: " + expected + "\n";
    std::size_t end = got.find('\n');
    got = end == std::string::npos ? got : got.substr(0, end + 1);
    if (got != want) {
        std::fprintf(stderr, "macro error: %s\n  want: %s  got:  %s", text.c_str(), want.c_str(),
                     got.c_str());
    }
    CHECK(results, got == want);
}

void NumberTests(TestResults& results) {
    ExpectValue(results, "(+ 2 (* 3 4))", "14");
    ExpectValue(results, "(- 0 7)", "-7");
    // / truncates toward zero and % takes the dividend's sign, as in Tisple
    ExpectValue(results, "(list (/ -7 2) (% -7 2) (/ 7 2))", "(-3 -1 3)");
    ExpectValue(results, "(list (& 12 10) (| 12 10) (^ 12 10) (~ 0) (<< 1 40) (>> -8 1))",
                "(8 14 6 -1 1099511627776 -4)");
    // integers are 64-bit: big ones are fine, overflow is an error
    ExpectValue(results, "(* 4294967295 4294967)", "18446742798104265");
    ExpectMacroError(results, "(macro v () (* (<< 1 62) 4))\n(const X u8 (v))",
                     "t.tsp:2:13: 4611686018427387904 * 4 does not fit in 64 bits; macro code at "
                     "t.tsp:1:13 (in macro v)");
    ExpectMacroError(results, "(macro v () (/ 1 0))\n(const X u8 (v))",
                     "t.tsp:2:13: division by zero: 1 / 0; macro code at t.tsp:1:13 (in macro v)");
    // floats: pi, sin, cos, sqrt; floor and round make integers again
    ExpectValue(results, "(round (* 1000 pi))", "3142");
    ExpectValue(results, "(list (round (sin (/ pi 2))) (round (cos pi)) (floor (sqrt 99)))",
                "(1 -1 9)");
    ExpectValue(results,
                "(list (round (/ 5 (float 2))) (round (/ -5 (float 2))) "
                "(floor (/ -5 (float 2))))",
                "(3 -3 -3)");
    ExpectValue(results, "(list (< 1 (/ 3 (float 2))) (= 2 (float 2)) (>= 2 3))",
                "(true true false)");
    // a float never goes into code
    ExpectMacroError(results, "(macro v () (/ 1 (float 2)))\n(const X u8 (v))",
                     "t.tsp:2:13: macro v: the float 0.5 can't go into code; write (round x) or "
                     "(floor x) (in macro v)");
}

void ListTests(TestResults& results) {
    ExpectValue(results, "(list 1 'a \"s\" :k true (list))", "(1 a \"s\" :k true ())");
    ExpectValue(results, "(len (range 0 10))", "10");
    ExpectValue(results, "(range 5 2)", "()");
    ExpectValue(results, "(nth (list 'a 'b 'c) 1)", "b");
    ExpectValue(results, "(head (list 1 2 3))", "1");
    ExpectValue(results, "(tail (list 1 2 3))", "(2 3)");
    ExpectValue(results, "(append (list 1) (list) (list 2 3))", "(1 2 3)");
    ExpectValue(results, "(repeat 3 '(f x))", "((f x) (f x) (f x))");
    ExpectValue(results, "(map (fn (x) (* x x)) (range 0 4))", "(0 1 4 9)");
    // a fn sees the lets around it when it is made
    ExpectValue(results, "(let k 10) (map (fn (x) (+ x k)) (list 1 2))", "(11 12)");
    // symbols from parts; strings to character codes
    ExpectValue(results, "(symbol 'sprite \"_\" 'x 2)", "sprite_x2");
    ExpectValue(results, "(chars \"AZ\")", "(65 90)");
    ExpectValue(results, "(len \"hello\")", "5");
    ExpectValue(results, "(map kind (list 1 (float 1) true 'a :b \"c\" (list) (fn () 1)))",
                "(int float bool symbol keyword string list fn)");
    // = compares forms
    ExpectValue(results, "(list (= '(a (b 1)) '(a (b 1))) (= 'a 'b) (!= \"x\" \"y\"))",
                "(true false true)");
    ExpectMacroError(results, "(macro v () (nth (list 1 2) 2))\n(const X u8 (v))",
                     "t.tsp:2:13: nth: index 2 is outside the 2-item list (1 2); macro code at "
                     "t.tsp:1:13 (in macro v)");
    ExpectMacroError(results, "(macro v () (head (list)))\n(const X u8 (v))",
                     "t.tsp:2:13: head of an empty list; macro code at t.tsp:1:13 (in macro v)");
    ExpectMacroError(results, "(macro v () (+ 'a 1))\n(const X u8 (v))",
                     "t.tsp:2:13: + takes integers, got the symbol a; macro code at t.tsp:1:13 (in "
                     "macro v)");
}

void FormTests(TestResults& results) {
    ExpectValue(results, "(let x 1) (set x (+ x 1)) x", "2");
    ExpectValue(results, "(if (< 1 2) 'yes 'no)", "yes");
    ExpectValue(results, "(let n 0) (while (< n 5) (set n (+ n 1))) n", "5");
    ExpectValue(results,
                "(let s 0) (for i 0 10 (if (= i 3) (continue)) (if (= i 6) (break)) "
                "(set s (+ s i))) s",
                "12");
    ExpectValue(results, "(match 'b ('a 1) ('b 2) (_ 3))", "2");
    ExpectValue(results, "(match 7 (1 'one) (_ 'many))", "many");
    ExpectValue(results, "(and (< 1 2) (not false))", "true");
    ExpectValue(results, "(for i 0 3 (if (= i 1) (return 'early))) 'late", "early");
    // a quasiquote builds a form; , drops in any value and ,@ splices a list
    ExpectValue(results, "(let xs (list 1 2)) `(f ,(len xs) ,@xs ,@(map (fn (x) (* 10 x)) xs))",
                "(f 2 1 2 10 20)");
    // recursion: macros call macros, and themselves
    ExpectValue(results, "(fib 15)", "610",
                "(macro fib (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))\n");
    // a macro called from macro code gets values; with & the rest come as a list
    ExpectValue(results, "(sum 1 2 3 4)", "10",
                "(macro sum (& xs) (let s 0) (for i 0 (len xs) (set s (+ s (nth xs i)))) s)\n");
    ExpectValue(results, "(map twice (list 1 2))", "(2 4)", "(macro twice (x) (* 2 x))\n");
    ExpectMacroError(results, "(macro v () (let x u8 5) x)\n(const X u8 (v))",
                     "t.tsp:2:13: macro code has no types; write (let x value); macro code at "
                     "t.tsp:1:13 (in macro v)");
    ExpectMacroError(results, "(macro v () (for i u8 0 3) 1)\n(const X u8 (v))",
                     "t.tsp:2:13: macro code has no types; write (for i start end ...); macro code "
                     "at t.tsp:1:13 (in macro v)");
    ExpectMacroError(results, "(macro v () (if 1 2 3))\n(const X u8 (v))",
                     "t.tsp:2:13: if needs true or false, got 1; compare it: (= x 0); macro code "
                     "at t.tsp:1:17 (in macro v)");
    ExpectMacroError(results, "(macro v () (match 3 (1 1)))\n(const X u8 (v))",
                     "t.tsp:2:13: no clause matches 3; add a last clause (_ body ...); macro code "
                     "at t.tsp:1:13 (in macro v)");
    ExpectMacroError(results, "(macro v () (nope 1))\n(const X u8 (v))",
                     "t.tsp:2:13: unknown function nope in macro v; macro code calls macros, fns "
                     "and list, map, range, ...; macro code at t.tsp:1:14 (in macro v)");
    ExpectMacroError(results, "(macro v () (make Thing))\n(const X u8 (v))",
                     "t.tsp:2:13: make is Tisple code, not macro code; build it in a template: "
                     "`(make ...); macro code at t.tsp:1:13 (in macro v)");
    ExpectMacroError(results, "(macro v () (let x 1))\n(const X u8 (v))",
                     "t.tsp:2:13: macro v: it gives nothing to paste in; end its body with the "
                     "forms, like `(...) (in macro v)");
    // (error ...) reports at the call
    ExpectMacroError(results, "(macro v () (error \"no \" 'good \" \" 42))\n(const X u8 (v))",
                     "t.tsp:2:13: no good 42 (in macro v)");
}

void LimitTests(TestResults& results) {
    ExpectMacroError(results, "(macro spin () (while true) 1)\n(const X u8 (spin))",
                     "t.tsp:2:13: macro spin ran more than 10000000 steps; is a loop missing its "
                     "end? (in macro spin)");
    ExpectMacroError(results, "(macro deep (n) (deep (+ n 1)))\n(const X u8 (deep 0))",
                     "t.tsp:2:13: macro deep nested more than 200 calls deep; does its recursion "
                     "reach a base case? (in macro deep)");
    ExpectMacroError(results, "(macro big () (len (range 0 1000000)))\n(const X u8 (big))",
                     "t.tsp:2:13: range would make a list of 1000000 items; at most 131072; macro "
                     "code at t.tsp:1:20 (in macro big)");
}

// ordinary Tisple functions run with their declared types, so they wrap like the Apple II
void TypedCallTests(TestResults& results) {
    const std::string add = "(fn add ((x u8)) u8 (+ x 200))\n";
    ExpectValue(results, "(list (add 10) (add 100))", "(210 44)", add);
    ExpectValue(results, "(neg 5)", "-5", "(fn neg ((x i8)) i8 (- 0 x))\n");
    ExpectValue(results, "(big (- 0 1))", "65535", "(fn big ((x i16)) u16 (as u16 x))\n");
    ExpectValue(results, "(is_odd 3)", "true", "(fn is_odd ((x u8)) bool (= (& x 1) 1))\n");
    // a function calls others and reads constants and tables
    ExpectValue(results, "(at3)", "40",
                "(const T (array u8 4) (make (array u8 4) 10 20 30 40))\n"
                "(const K u8 3)\n(fn at3 () u8 (at T K))\n");
    // scalar constants read straight from macro code
    ExpectValue(results, "(* K 2)", "14", "(const K u8 7)\n");
    ExpectMacroError(results, add + "(macro v () (add 300))\n(const X u8 (v))",
                     "t.tsp:3:13: add takes ((x u8)); 300 does not fit in u8 (0 to 255); macro "
                     "code at t.tsp:2:13 (in macro v)");
    ExpectMacroError(results,
                     "(fn key () u8 (io_read u8 0xC000))\n(fn f () u8 (key))\n"
                     "(macro v () (f))\n(const X u8 (v))",
                     "t.tsp:4:13: f can't run at compile time: it calls t.key, which uses "
                     "io_read; macro code at t.tsp:3:13 (in macro v)");
    ExpectMacroError(results, "(let g u8 5)\n(fn f () u8 g)\n(macro v () (f))\n(const X u8 (v))",
                     "t.tsp:4:13: f can't run at compile time: it uses the top-level let t.g, "
                     "which only has a value at run time; macro code at t.tsp:3:13 (in macro v)");
    ExpectMacroError(results, "(fn f () u8 (+ 1 nope))\n(macro v () (f))\n(const X u8 (v))",
                     "t.tsp:3:13: f can't be used at compile time until it checks: t.tsp:1:18: "
                     "unknown name nope; macro code at t.tsp:2:13 (in macro v)");
}

void TableTests(TestResults& results) {
    // lib/tables: a table from a macro, a typed function, with placement after
    ExpectExpanded(results,
                   "(import tables)\n(macro sq (i) (* i i))\n(fn wrap ((i u8)) u8 (+ i 250))\n"
                   "(tables.table SQUARES u16 4 sq :boundary 256)\n"
                   "(tables.table WRAPS u8 8 wrap)",
                   "(import tables)\n\n(fn wrap ((i u8)) u8\n    (+ i 250))\n\n"
                   "(const SQUARES (array u16 4) (make (array u16 4) 0 1 4 9) :boundary 256)\n"
                   "(const WRAPS (array u8 8) (make (array u8 8) 250 251 252 253 254 255 0 1))\n");
    // sprites: 7 pixels a byte, the low bit leftmost
    ExpectExpanded(results, "(import tables)\n(tables.sprite BALL \".##.\" \"####\" \".##.\")",
                   "(import tables)\n(const BALL (array u8 3) (make (array u8 3) 6 15 6))\n");
    ExpectExpanded(results, "(import tables)\n(tables.sprite BAR \"########\" \"#......#\")",
                   "(import tables)\n(const BAR (array u8 4) (make (array u8 4) 127 1 1 1))\n");
    ExpectMacroError(results, "(import tables)\n(tables.sprite BALL \".#\" \"#\")",
                     "t.tsp:2:1: sprite BALL: row 1 is 1 wide, the first is 2 (in macro "
                     "tables.sprite)");
    ExpectMacroError(results, "(import tables)\n(tables.sprite BALL \".x\")",
                     "t.tsp:2:1: sprite rows are # and . only, got .x (in macro tables.sprite)");
    // a template's names resolve in the macro's module, argument names where they are written
    ExpectExpanded(results, "(import macros)\n(fn f () void (macros.unroll i 0 3 (g i)))",
                   "(import macros)\n\n(fn f () void\n    (do\n        (do\n            (g 0))\n"
                   "        (do\n            (g 1))\n        (do\n            (g 2))))\n");
}

// plasma's tables come from macros, and its unrolled draw runs the same on the 6502
void PlasmaTests(TestResults& results) {
    Compiled c = CompileFile("programs/plasma/main.tsp");
    CHECK(results, c.ok);
    if (!c.ok) {
        std::fprintf(stderr, "plasma: %s\n", c.errors.c_str());
        return;
    }
    const tisple::Program& program = c.built.program;
    std::int32_t sine = tisple::FindVar(program, "main.SINE");
    std::int32_t ramp = tisple::FindVar(program, "main.RAMP_HI");
    CHECK(results, sine >= 0 && ramp >= 0);
    if (sine < 0 || ramp < 0) {
        return;
    }
    const std::vector<std::uint8_t>& table = program.vars[static_cast<std::size_t>(sine)].init;
    bool same = table.size() == 256;
    for (std::size_t i = 0; same && i < 256; i++) {
        double angle = 2.0 * std::numbers::pi * static_cast<double>(i) / 256.0;
        same = table[i] == static_cast<std::uint8_t>(std::round(128.0 + 127.0 * std::sin(angle)));
    }
    CHECK(results, same);
    const std::vector<std::uint8_t>& high = program.vars[static_cast<std::size_t>(ramp)].init;
    CHECK(results, high.size() == 256 && high[0x10] == 0x20 && high[0xC5] == 0x10);
    Diff d;
    CHECK(results, StartDiff(d, c));
    CheckDiff(results, d, "main.draw", {});
    CheckDiff(results, d, "main.draw", {});
    // the screen is not a global, so compare it here
    bool screen = true;
    for (std::uint32_t a = 0x400; a < 0x800; a++) {
        screen = screen && d.in.memory[a] == d.sim.machine->mem[a];
    }
    CHECK(results, screen);
}

} // namespace

void RunMacroTests(TestResults& results) {
    NumberTests(results);
    ListTests(results);
    FormTests(results);
    LimitTests(results);
    TypedCallTests(results);
    TableTests(results);
    PlasmaTests(results);
}

} // namespace tisple_tests
