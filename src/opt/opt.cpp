#include "backend/analysis.hpp"
#include "opt/opt_internal.hpp"

#include <cstdlib>
#include <cstring>

namespace tisple {

namespace opt {

const Expr& At(const Pass& p, ExprId id) {
    return p.program->exprs[static_cast<std::size_t>(id)];
}

ExprId Item(const Pass& p, const Expr& e, std::uint32_t i) {
    return p.program->lists[e.first + i];
}

const Var& VarOf(const Pass& p, std::int32_t var) {
    return p.program->vars[static_cast<std::size_t>(var)];
}

bool Allow(Pass& p, const char* what, ExprId at, std::int32_t var) {
    std::int64_t budget = p.options->bisect;
    if (budget >= 0 && p.report->applied >= static_cast<std::uint64_t>(budget)) {
        return false;
    }
    p.report->applied++;
    p.last_what = what;
    p.last_at = at;
    p.last_var = var;
    return true;
}

} // namespace opt

namespace {

// a fixed point comes long before this; the cap only guards against a pass that undoes
// another's work
constexpr int kMaxRounds = 64;

bool Disabled(const OptOptions& options, const std::string& name) {
    for (const std::string& pass : options.disabled) {
        if (pass == name) {
            return true;
        }
    }
    return false;
}

void Run(Program& program, const OptOptions& options, OptReport& report) {
    opt::Pass p;
    p.program = &program;
    p.options = &options;
    p.report = &report;
    bool dead = !Disabled(options, "dead");
    bool loops = !Disabled(options, "loops");
    bool consts = !Disabled(options, "consts");
    bool licm = !Disabled(options, "licm");
    bool cse = !Disabled(options, "cse");
    bool eval = !Disabled(options, "eval");
    bool collapse = !Disabled(options, "collapse");
    bool ranges = !Disabled(options, "ranges");
    bool args = !Disabled(options, "args");
    bool returns = !Disabled(options, "returns");
    bool vars = !Disabled(options, "vars");
    bool inlines = !Disabled(options, "inline");
    bool elements = !Disabled(options, "elements");
    // unrolling (in collapse's walk) waits for the other passes to settle: a body they shorten
    // may collapse, and its copies would not
    bool unroll = collapse && !Disabled(options, "unroll");
    bool unswitch = collapse && !Disabled(options, "unswitch");
    std::size_t fn_count = program.functions.size();
    for (int round = 0; round < kMaxRounds; round++) {
        opt::CountRefs(p);
        opt::ComputeSummaries(p);
        opt::ComputeVarRanges(p, vars, elements);
        std::uint64_t changes = 0;
        for (std::size_t f = 0; f < fn_count && dead; f++) {
            changes += opt::EditStatements(p, static_cast<std::int32_t>(f), opt::DecideDead);
        }
        for (std::size_t f = 0; f < fn_count && loops; f++) {
            changes += opt::EditStatements(p, static_cast<std::int32_t>(f), opt::DecideLoop);
        }
        if (args) {
            changes += opt::DropUnreadArgs(p);
        }
        if (returns) {
            changes += opt::DropUnusedReturns(p);
        }
        if (collapse) {
            changes += opt::CollapseLoops(p);
        }
        // before constant propagation, which carries the values on
        if (eval) {
            changes += opt::EvaluateCalls(p);
        }
        if (consts) {
            changes += opt::PropagateConstants(p);
        }
        if (ranges) {
            changes += opt::FoldRanges(p);
        }
        // last: they add variables the summaries of this round do not know (locals only, so
        // what the summaries say of globals still holds). Common subexpressions after code
        // motion, which works out the same value twice in a loop once already
        for (std::size_t f = 0; f < fn_count && licm; f++) {
            changes += opt::HoistInvariants(p, static_cast<std::int32_t>(f));
        }
        for (std::size_t f = 0; f < fn_count && cse; f++) {
            changes += opt::CommonSubexpressions(p, static_cast<std::int32_t>(f));
        }
        if (changes == 0 && inlines) {
            // settled: the functions are as small as the passes make them, which decides
            // which the backend would inline (it looks at the program they leave); the rounds
            // after work on the copies. Before unrolling, which leaves loops with calls alone
            changes = opt::InlineCalls(p);
        }
        if (changes == 0 && (unroll || unswitch)) {
            // settled: what this round worked out still holds, and only unrolling and
            // unswitching are left to change anything; the rounds after them fold the copies
            p.unroll = unroll;
            p.unswitch = unswitch;
            changes = opt::CollapseLoops(p);
            p.unroll = false;
            p.unswitch = false;
        }
        if (changes == 0) {
            break;
        }
        // literals put in for variables join the constant chains around them: (+ 65535 v) is
        // (- v 1) again, the shape the backend knows
        FoldConstants(program);
    }
    if (p.last_at < 0) {
        return;
    }
    report.last = p.last_what;
    if (p.last_var >= 0) {
        report.last += " " + program.vars[static_cast<std::size_t>(p.last_var)].name;
    }
    report.last += " at " + FormatLoc(program.sources, opt::At(p, p.last_at).loc);
}

} // namespace

const std::vector<std::string>& OptPassNames() {
    static const std::vector<std::string> names = {
        "inline", "dead", "loops", "args", "returns",  "collapse", "unroll", "unswitch",
        "consts", "ranges", "vars", "elements", "licm",     "cse",    "eval"};
    return names;
}

void OptimizeProgram(Program& program, const OptOptions& options, OptReport& report) {
    report = OptReport();
    if (options.level <= 0) {
        return;
    }
    if (options.bisect >= 0) {
        // how many there are without the limit, so a bisection knows its range
        Program copy = program;
        OptOptions all = options;
        all.bisect = -1;
        OptReport full;
        Run(copy, all, full);
        report.total = full.applied;
    }
    Run(program, options, report);
}

bool ParseOptFlag(int argc, char** argv, int& i, OptOptions& options, std::string& error) {
    const char* arg = argv[i];
    if (std::strcmp(arg, "-O0") == 0 || std::strcmp(arg, "-O1") == 0) {
        options.level = arg[2] - '0';
        return true;
    }
    bool bisect = std::strcmp(arg, "--opt-bisect") == 0;
    bool no_pass = std::strcmp(arg, "--no-pass") == 0;
    if (!bisect && !no_pass) {
        return false;
    }
    if (i + 1 >= argc) {
        error = std::string(arg) + " needs a value";
        return true;
    }
    const char* value = argv[++i];
    if (bisect) {
        char* end = nullptr;
        long long n = std::strtoll(value, &end, 10);
        if (*value == '\0' || *end != '\0' || n < 0) {
            error = "--opt-bisect takes a count of transformations, 0 or more";
        }
        options.bisect = n;
        return true;
    }
    for (const std::string& name : OptPassNames()) {
        if (name == value) {
            options.disabled.push_back(name);
            return true;
        }
    }
    error = std::string("no pass ") + value + "; the passes are";
    for (const std::string& name : OptPassNames()) {
        error += " " + name;
    }
    return true;
}

std::string DescribeOptReport(const OptReport& report) {
    std::string out = "opt-bisect: " + std::to_string(report.applied) + " of " +
                      std::to_string(report.total) + " transformations";
    if (!report.last.empty()) {
        out += "; the last: " + report.last;
    }
    return out + "\n";
}

} // namespace tisple
