#include "check/expand.hpp"
#include "reader/format.hpp"
#include "test_check.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

// the entry module after expansion, through the formatter
std::string ExpandText(const std::string& text) {
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

void ExpectExpansion(TestResults& results, const std::string& text, const std::string& expected) {
    std::string got = ExpandText(text);
    if (got != expected) {
        std::fprintf(stderr, "expand: %s\n  want: %s  got:  %s", text.c_str(), expected.c_str(),
                     got.c_str());
    }
    CHECK(results, got == expected);
}

void TemplateTests(TestResults& results) {
    const std::string swap =
        "(macro swap (a b) `(do (let tmp# u8 ,a) (set ,a ,b) (set ,b tmp#)))\n";
    // tmp# is fresh per expansion, the same name within one
    ExpectExpansion(results,
                    swap + "(fn f () void (let x u8 1) (let y u8 2) (swap x y) (swap y x))",
                    "(fn f () void\n"
                    "    (let x u8 1)\n"
                    "    (let y u8 2)\n"
                    "    (do\n"
                    "        (let tmp__1 u8 x)\n"
                    "        (set x y)\n"
                    "        (set y tmp__1))\n"
                    "    (do\n"
                    "        (let tmp__2 u8 y)\n"
                    "        (set y x)\n"
                    "        (set x tmp__2)))\n");
    // ,@ splices a list of forms; macros inside arguments expand too
    ExpectExpansion(results,
                    "(macro inc (p) `(set ,p (+ ,p 1)))\n"
                    "(macro twice (body) `(do ,@body ,@body))\n"
                    "(fn f () void (let x u8 0) (twice ((inc x))))",
                    "(fn f () void\n"
                    "    (let x u8 0)\n"
                    "    (do\n"
                    "        (set x (+ x 1))\n"
                    "        (set x (+ x 1))))\n");
    // names, fields and match patterns are not calls even when spelled like a macro
    ExpectExpansion(results,
                    "(macro x (a) `(+ ,a 1))\n"
                    "(struct P ((x u8)))\n"
                    "(fn f ((x u8)) void (match x (0 (x 1)) (_)))",
                    "(struct P ((x u8)))\n\n"
                    "(fn f ((x u8)) void\n"
                    "    (match x\n"
                    "        (0 (+ 1 1))\n"
                    "        (_)))\n");
}

void ErrorTests(TestResults& results) {
    const std::string inc = "(macro inc (p) `(set ,p (+ ,p 1)))\n";
    ExpectError(results, inc + "(fn f () void (let x u8 0) (inc x x))",
                "t.tsp:2:28: inc takes 1 argument, got 2; write (inc p)");
    ExpectError(results, "(macro m (a) `(+ ,b 1))\n(fn f () u8 (m 1))",
                "t.tsp:2:13: only a macro argument can be unquoted; write ,name with a name from "
                "(a) (in macro m)");
    ExpectError(results, "(macro m (body) `(do ,@body))\n(fn f () void (m 5))",
                "t.tsp:2:18: ,@body needs a list of forms; pass ((f a) (g b))");
    ExpectError(results, "(macro m (a) a)",
                "t.tsp:1:14: a macro body is one ` template; write `(...)");
    ExpectError(results, "(macro m (a) `(m ,a))\n(fn f () void (m 1))",
                "t.tsp:2:15: macro m expands more than 64 levels deep; it probably expands to "
                "itself (in macro m)");
    ExpectError(results, "(macro m (a) `(do ,@a))\n(macro m (a) `,a)",
                "t.tsp:2:1: macro m is defined twice; rename one");
    ExpectError(results, "(macro inc (p) `(set ,p 1))\n(fn inc () void)",
                "t.tsp:2:5: inc is declared twice in this module (a macro); rename one");
    // an error in template code points at the call and names the macro
    ExpectError(results, "(macro add_big (a) `(+ ,a 300))\n(fn f ((x u8)) u8 (add_big x))",
                "t.tsp:2:19: 300 does not fit in u8; write (+ (as u16 x) 300) (in macro add_big)");
    ExpectError(results,
                "(macro inner (a) `(+ ,a 300))\n(macro outer (a) `(inner ,a))\n"
                "(fn f ((x u8)) u8 (outer x))",
                "t.tsp:3:19: 300 does not fit in u8; write (+ (as u16 x) 300) (in macro inner from "
                "macro outer)");
    // an error in an argument points at the argument itself
    ExpectError(results, inc + "(fn f () void (inc (+ 1 nope)))", "t.tsp:2:25: unknown name nope");
}

} // namespace

void RunExpandTests(TestResults& results) {
    TemplateTests(results);
    ErrorTests(results);
}

} // namespace tisple_tests
