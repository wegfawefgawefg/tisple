#include "test_check.hpp"

#include <cstdio>

namespace tisple_tests {

void RunCheckExampleTests(TestResults& results) {
    const char* const files[] = {
        "examples/fill.tsp",       "examples/hires_plot.tsp",      "examples/keys.tsp",
        "examples/macros.tsp",     "examples/read_key.tsp",        "examples/speaker.tsp",
        "examples/sprites.tsp",    "examples/dot/main.tsp",        "examples/dot/gfx/hgr.tsp",
        "examples/multi/main.tsp", "examples/multi/gfx/lores.tsp", "lib/apple2.tsp",
        "lib/lores.tsp",           "programs/life/main.tsp",       "programs/snake/main.tsp",
    };
    for (const char* file : files) {
        Built built = BuildFile(file);
        if (!built.ok) {
            std::fprintf(stderr, "%s does not check:\n%s", file, ErrorText(built).c_str());
        }
        CHECK(results, built.ok);
    }

    // imports: the dot program pulls in gfx/hgr from its folder and apple2 from lib/
    Built dot = BuildFile("examples/dot/main.tsp");
    CHECK(results, dot.program.modules.size() == 3);
    CHECK(results, tisple::FindFunction(dot.program, "gfx/hgr.init") >= 0);
    CHECK(results, tisple::FindFunction(dot.program, "main.main") == dot.program.main);

    // every expression has a resolved type
    bool typed = true;
    for (const tisple::Expr& expr : dot.program.exprs) {
        typed = typed && expr.type != tisple::kErrorType;
    }
    CHECK(results, typed);
}

} // namespace tisple_tests
