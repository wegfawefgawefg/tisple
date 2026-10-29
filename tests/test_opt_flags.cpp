// -O0, -O1, --opt-bisect and --no-pass: bisecting stops after exactly N transformations,
// every prefix (of every pass's work) compiles to code the interpreter agrees with, and 0 is
// the same code as -O0

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

// constants through branches and a match, divisions and a product a loop never changes (one
// for may run zero times, so it gets a guard), (- x x)
const char* const kMotion =
    "(let g u8 0)\n(let h u8 0)\n(let gw u16 0)\n(let a (array u8 8))\n"
    "(fn f ((x u8)) u8\n"
    "    (let k u8 3) (let z u8 0)\n"
    "    (if (> k 2) (set g (/ x k)) (set g 9))\n"
    "    (for i u8 0 x (set (at a (& i 7)) (+ (at a (& i 7)) (/ 200 (| x 1)))) (set h (% x 7)))\n"
    "    (let c u8 0) (while (< c 4) (set c (+ c 1)) (set gw (+ gw (* (as u16 x) 300))))\n"
    "    (match k (3 (set h (+ h (- x x)))) (_ (set h 0)))\n"
    "    (+ g (/ h (| z 1))))\n"
    "(fn k ((x u8)) u8 (let s u8 0) (for i u8 0 5 (set s (+ s (/ x 3)))) s)\n";

// values worked out twice (a division and its remainder apart), a call worked out at compile
// time, what a condition says
const char* const kCommon =
    "(let g u16 0)\n(let h u16 0)\n(let gb u8 0)\n"
    "(fn tri ((n u8)) u16 (let s u16 0) (for i u8 0 n (set s (+ s (as u16 i)))) s)\n"
    "(fn f ((x u8)) u8\n"
    "    (let w u16 (as u16 x)) (set g (+ (* w (+ w 3)) (tri 9)))\n"
    "    (if (> x 9) (set gb 1)) (set h (- (* w (+ w 3)) 1))\n"
    "    (set g (+ g (/ w 7))) (if (= x 3) (set gb (+ x 1))) (set h (+ h (% w 7)))\n"
    "    (match x (4 (set gb (* x 5))) (_ (do)))\n"
    "    (+ gb (as u8 (+ g h))))\n"
    "(fn k ((x u8)) u8 (as u8 (+ (tri 12) (as u16 x))))\n";

struct Call {
    const char* function;
    std::uint16_t arg;
};

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
          Parses({"--no-pass", "fold"}, o, error) &&
              error.find("dead loops args returns collapse unroll unswitch consts ranges vars "
                         "elements licm cse eval") != std::string::npos);
    error.clear();
    CHECK(results, Parses({"--opt-bisect", "-3"}, o, error) && !error.empty());
    error.clear();
    CHECK(results, Parses({"--opt-bisect"}, o, error) && !error.empty());
    error.clear();
    CHECK(results, !Parses({"--asm"}, o, error));
}

// every prefix of the transformations compiles to code the interpreter agrees with
void Bisect(TestResults& results, const char* program, const std::vector<Call>& calls,
            std::uint64_t at_least) {
    Opted all = Optimize(program);
    std::uint64_t total = all.report.applied;
    CHECK(results, total >= at_least);
    // the same run twice makes the same transformations
    CHECK(results, Optimize(program).report.last == all.report.last);
    Built built = BuildText(program);
    bool ok = false;
    tisple::OptOptions none;
    none.level = 0;
    tisple::Compiled o0 = CompileWith(built, none, ok);
    CHECK(results, ok);
    for (std::uint64_t n = 0; n <= total + 1; n++) {
        tisple::OptOptions opt;
        opt.bisect = static_cast<std::int64_t>(n);
        Opted part = Optimize(program, opt);
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
        bool same = StartDiff(d, c);
        for (const Call& call : calls) {
            same = same && DiffCall(d, call.function, {call.arg});
        }
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
    std::string line = tisple::DescribeOptReport(Optimize(program, one).report);
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

    // (collapse would run the for's body once)
    tisple::OptOptions no_loops;
    no_loops.disabled = {"loops", "collapse"};
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

    // constants keep their if and match without consts; the divisions stay in their loops
    // without licm
    tisple::OptOptions no_consts;
    no_consts.disabled = {"consts"};
    Opted kept = Optimize(kMotion, no_consts);
    CHECK(results, CountKind(kept, "t.f", ExprKind::Match) == 1);
    CHECK(results, CountKind(Optimize(kMotion), "t.f", ExprKind::Match) == 0);
    // (collapse would add the loops' products up at once)
    tisple::OptOptions no_licm;
    no_licm.disabled = {"licm", "collapse"};
    int division = static_cast<int>(tisple::Op::Div);
    CHECK(results, LoopDepth(Optimize(kMotion, no_licm), "t.k", ExprKind::Binary, division) == 1);
    CHECK(results, LoopDepth(Optimize(kMotion), "t.k", ExprKind::Binary, division) == 0);
    CHECK(results, LoopDepth(Optimize(kMotion), "t.f", ExprKind::Binary, division) == 0);

    // the products stay two without cse; tri stays a call without eval
    tisple::OptOptions no_cse;
    no_cse.disabled = {"cse"};
    CHECK(results, CountKind(Optimize(kCommon, no_cse), "t.f", ExprKind::Let) == 1);
    CHECK(results, CountKind(Optimize(kCommon), "t.f", ExprKind::Let) == 4);
    tisple::OptOptions no_eval;
    no_eval.disabled = {"eval"};
    CHECK(results, CountKind(Optimize(kCommon, no_eval), "t.k", ExprKind::Call) == 1);
    CHECK(results, CountKind(Optimize(kCommon), "t.k", ExprKind::Call) == 0);
}

} // namespace

void RunOptFlagTests(TestResults& results) {
    Flags(results);
    Bisect(results, kProgram, {{"t.f", 7}, {"t.f", 200}, {"t.k", 9}}, 8);
    Bisect(results, kMotion, {{"t.f", 7}, {"t.f", 0}, {"t.f", 200}, {"t.k", 9}}, 12);
    Bisect(results, kCommon, {{"t.f", 3}, {"t.f", 4}, {"t.f", 200}, {"t.k", 9}}, 6);
    PassesOff(results);
}

} // namespace tisple_tests
