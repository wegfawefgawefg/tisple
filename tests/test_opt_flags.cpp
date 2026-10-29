// -O0, -O1, --opt-bisect and --no-pass: bisecting stops after exactly N transformations,
// every prefix compiles to code the interpreter agrees with, and 0 is the same code as -O0

#include "test_backend.hpp"
#include "test_opt.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace tisple_tests {

namespace {

using tisple::ExprKind;

// dead lets, stores, statements and loops, in several functions
const char* const kProgram =
    "(let g u8 0)\n(let h u8 0)\n(let a (array u8 8))\n"
    "(fn sq ((v u8)) u8 (* v v))\n"
    "(fn f ((x u8)) u8\n"
    "    (let d u8 (/ x 3)) (set g x) (set g (+ x 1)) (sq x) (set h h)\n"
    "    (for i u8 0 4 (let j u8 0) (while (< j 3) (set j (+ j 1))) (let l u8 (% i 5)))\n"
    "    (let y u8 1) (set y (+ x 2)) (+ y g))\n"
    "(fn k ((x u8)) u8 (let c u8 x) (while (> c 0) (set c (- c 1))) (set (at a 1) x) x)\n";

tisple::Compiled CompileWith(const Built& built, const tisple::OptOptions& opt, bool& ok) {
    tisple::BuildOptions options;
    options.keep_all_functions = true;
    options.opt = opt;
    tisple::Compiled code;
    std::vector<tisple::Diagnostic> errors;
    ok = built.ok && tisple::CompileProgram(built.program, options, code, errors);
    return code;
}

bool Parses(std::vector<std::string> words, tisple::OptOptions& options, std::string& error) {
    std::vector<char*> argv;
    for (std::string& w : words) {
        argv.push_back(w.data());
    }
    int argc = static_cast<int>(argv.size());
    for (int i = 0; i < argc; i++) {
        if (!tisple::ParseOptFlag(argc, argv.data(), i, options, error)) {
            return false;
        }
    }
    return true;
}

void Flags(TestResults& results) {
    tisple::OptOptions o;
    std::string error;
    CHECK(results, Parses({"-O0"}, o, error) && o.level == 0 && error.empty());
    CHECK(results, Parses({"-O1", "--opt-bisect", "12"}, o, error) && o.level == 1);
    CHECK(results, o.bisect == 12 && error.empty());
    CHECK(results, Parses({"--no-pass", "loops"}, o, error) && error.empty());
    CHECK(results, o.disabled.size() == 1 && o.disabled[0] == "loops");
    CHECK(results,
          Parses({"--no-pass", "fold"}, o, error) && error.find("dead loops") != std::string::npos);
    error.clear();
    CHECK(results, Parses({"--opt-bisect", "-3"}, o, error) && !error.empty());
    error.clear();
    CHECK(results, Parses({"--opt-bisect"}, o, error) && !error.empty());
    error.clear();
    CHECK(results, !Parses({"--asm"}, o, error));
}

void Bisect(TestResults& results) {
    Opted all = Optimize(kProgram);
    std::uint64_t total = all.report.applied;
    CHECK(results, total >= 8);
    // the same run twice makes the same transformations
    CHECK(results, Optimize(kProgram).report.last == all.report.last);
    Built built = BuildText(kProgram);
    bool ok = false;
    tisple::OptOptions none;
    none.level = 0;
    tisple::Compiled o0 = CompileWith(built, none, ok);
    CHECK(results, ok);
    for (std::uint64_t n = 0; n <= total + 1; n++) {
        tisple::OptOptions opt;
        opt.bisect = static_cast<std::int64_t>(n);
        Opted part = Optimize(kProgram, opt);
        CHECK(results, part.report.applied == (n < total ? n : total));
        CHECK(results, part.report.total == total);
        Compiled c;
        c.built = built;
        c.code = CompileWith(built, opt, c.ok);
        CHECK(results, c.ok);
        if (n == 0) {
            CHECK(results, c.code.image == o0.image);
        }
        Diff d;
        bool same = StartDiff(d, c) && DiffCall(d, "t.f", {7}) && DiffCall(d, "t.f", {200}) &&
                    DiffCall(d, "t.k", {9});
        if (!same) {
            std::fprintf(stderr, "--opt-bisect %llu: %s\n", static_cast<unsigned long long>(n),
                         d.message.c_str());
        }
        CHECK(results, same);
    }
    // the description names what, which variable and where
    CHECK(results, all.report.last.find(" at t.tsp:") != std::string::npos);
    tisple::OptOptions one;
    one.bisect = 1;
    std::string line = tisple::DescribeOptReport(Optimize(kProgram, one).report);
    CHECK(results,
          line.rfind("opt-bisect: 1 of " + std::to_string(total) + " transformations; the last: ",
                     0) == 0);
}

void PassesOff(TestResults& results) {
    tisple::OptOptions none;
    none.level = 0;
    Opted o0 = Optimize(kProgram, none);
    CHECK(results, o0.report.applied == 0);
    CHECK(results, CountKind(o0, "t.f", ExprKind::While) == 1);
    CHECK(results, CountKind(o0, "t.f", ExprKind::Let) == 4);

    tisple::OptOptions no_loops;
    no_loops.disabled = {"loops"};
    Opted dead = Optimize(kProgram, no_loops);
    CHECK(results, CountKind(dead, "t.f", ExprKind::While) == 1);
    CHECK(results, CountKind(dead, "t.f", ExprKind::For) == 1);
    // d, l and y's first value gone; j is read by the while the loops pass would remove
    CHECK(results, CountKind(dead, "t.f", ExprKind::Let) == 1);

    tisple::OptOptions no_dead;
    no_dead.disabled = {"dead"};
    Opted loops = Optimize(kProgram, no_dead);
    CHECK(results, CountKind(loops, "t.f", ExprKind::For) == 0);
    CHECK(results, CountKind(loops, "t.k", ExprKind::While) == 0);
    CHECK(results, CountKind(loops, "t.k", ExprKind::Let) == 1); // c stays: dead is off

    Opted both = Optimize(kProgram);
    CHECK(results, CountKind(both, "t.f", ExprKind::Let) == 0);
    CHECK(results, CountKind(both, "t.k", ExprKind::Let) == 0);
}

} // namespace

void RunOptFlagTests(TestResults& results) {
    Flags(results);
    Bisect(results);
    PassesOff(results);
}

} // namespace tisple_tests
