// room for a program that does not fit (backend/squeeze.hpp): the size budget tightens until
// it does and says what it gave up; a program that fits is built as usual; --no-inline's
// names must be functions

#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

// one stretch, $6000-$94FF: no memory below the origin, no language card
tisple::BuildOptions OneStretch() {
    tisple::BuildOptions options;
    options.low_start = options.origin;
    options.use_card = false;
    return options;
}

Compiled CompileWith(const std::string& text, const tisple::BuildOptions& options) {
    Compiled c;
    c.built = BuildText(text);
    if (!c.built.ok) {
        c.errors = ErrorText(c.built);
        return c;
    }
    std::vector<tisple::Diagnostic> errors;
    c.ok = tisple::CompileProgram(c.built.program, options, c.code, errors);
    c.errors = tisple::FormatDiagnostics(c.built.program.sources, errors);
    return c;
}

std::string Warnings(const Compiled& c) {
    return tisple::FormatDiagnostics(c.built.program.sources, c.code.warnings);
}

bool Runs(const Compiled& c) {
    Diff d;
    d.in.max_steps = 2000000;
    d.max_cycles = 40000000;
    bool ok = StartDiff(d, c) && DiffCall(d, "t.main", {});
    if (!ok) {
        std::fprintf(stderr, "squeezed program: %s\n", d.message.c_str());
    }
    return ok;
}

// the program with a table of n bytes that fills the rest of memory
std::string WithTable(const char* program, std::uint32_t n) {
    return "(let filler (array u8 " + std::to_string(n) + "))\n" + program;
}

// a byte table one byte bigger at a time from where the usual build fits, until it does not:
// then the squeezed build must fit, run as the interpreter does and say what it traded
void CheckSqueezes(TestResults& results, const char* program, const char* traded) {
    tisple::BuildOptions options = OneStretch();
    Compiled usual = CompileWith(WithTable(program, 1), options);
    CHECK(results, usual.ok && Warnings(usual).empty());
    if (!usual.ok) {
        std::fprintf(stderr, "%s\n", usual.errors.c_str());
        return;
    }
    // what is left in $6000-$94FF, then up from just under that until the usual build fails
    std::uint32_t used = static_cast<std::uint32_t>(usual.code.image.size());
    std::uint32_t n = 0x9500 - 0x6000 - used - 16;
    for (int tries = 0; tries < 400; tries++, n += 8) {
        Compiled c = CompileWith(WithTable(program, n), options);
        std::string notes = Warnings(c);
        if (c.ok && notes.empty()) {
            continue; // fits as usual
        }
        bool said =
            notes.find("does not fit its memory with the usual size budget") != std::string::npos &&
            notes.find(traded) != std::string::npos;
        if (!c.ok || !said) {
            std::fprintf(stderr, "table of %u bytes: %s%s\n", n, c.errors.c_str(), notes.c_str());
        }
        CHECK(results, c.ok && said);
        CHECK(results, c.ok && c.code.origin + c.code.image.size() <= 0x9500);
        CHECK(results, c.ok && Runs(c));
        return;
    }
    CHECK(results, false); // never ran out of room
}

// a multiply in a loop pulls in the quarter-square routines and their 1 KB of tables; with
// no room for them, shift and add
const char* const kMultiplies =
    "(let a (array u8 64))\n"
    "(let b (array u8 64))\n"
    "(fn main () u16\n"
    "    (let sum u16 0)\n"
    "    (for i u8 0 64 (set (at a i) (+ i 3)) (set (at b i) (* i 5)))\n"
    "    (for i u8 0 64\n"
    "        (set sum (+ sum (* (as u16 (at a i)) (as u16 (at b i))))))\n"
    "    sum)\n";

// a small function copied at each of its calls (each copy bigger than a JSR); with no room for
// the copies, calls
const char* const kCopies = "(let out (array u8 64))\n"
                            "(let k u8 1)\n"
                            "(fn put ((i u8) (v u8)) void\n"
                            "    (set k (+ k (>> v 1)))\n"
                            "    (set (at out i) (^ (+ v k) (at out (+ i 1)))))\n"
                            "(fn main () u8\n"
                            "    (put (io_read u8 0xC010) (io_read u8 0xC000)) (put (io_read u8 "
                            "0xC011) (io_read u8 0xC001))\n"
                            "    (put (io_read u8 0xC012) (io_read u8 0xC002)) (put (io_read u8 "
                            "0xC013) (io_read u8 0xC003))\n"
                            "    (put (io_read u8 0xC014) (io_read u8 0xC004))\n"
                            "    (at out 7))\n";

void TestSqueeze(TestResults& results) {
    CheckSqueezes(results, kMultiplies, "Multiplies by shift and add");
    CheckSqueezes(results, kCopies, "Called instead of copied: 1 function (t.put)");
    // a program that cannot fit even so says it tried
    Compiled big = CompileWith(WithTable(kMultiplies, 20000), OneStretch());
    CHECK(results,
          !big.ok && big.errors.find("even with the tightest size budget") != std::string::npos);
}

void TestNoInlineNames(TestResults& results) {
    const char* program = "(fn helper ((x u8)) u8 (+ x 1))\n"
                          "(fn main () u8 (helper (io_read u8 0xC000)))\n";
    tisple::BuildOptions options;
    options.no_inline = {"t.helper"};
    CHECK(results, CompileWith(program, options).ok);
    options.no_inline = {"helper"};
    Compiled meant = CompileWith(program, options);
    CHECK(results,
          !meant.ok && meant.errors.find("--no-inline helper: no function has that "
                                         "name; did you mean t.helper?") != std::string::npos);
    options.no_inline = {"t.nothing"};
    Compiled none = CompileWith(program, options);
    CHECK(results,
          !none.ok && none.errors.find("the names it takes are a module's path, a dot "
                                       "and the function, like t.helper") != std::string::npos);
}

} // namespace

void RunBackendSqueezeTests(TestResults& results) {
    TestSqueeze(results);
    TestNoInlineNames(results);
}

} // namespace tisple_tests
