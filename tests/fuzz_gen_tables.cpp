#include "fuzz_gen.hpp"
#include "fuzz_gen_internal.hpp"

// Jump tables and calls through pointers (backend gen_match.cpp, dispatch.cpp): matches of 5
// to 24 dense clauses whose bodies are statements (the 6502 pushes an address and returns
// into it, the 65C02 jumps through JMP (table,X)), some inside a counted loop that may keep
// its counter in X (then the 65C02 pushes too), some sparse or on an enum; and calls through
// a table of function pointers (the 65C02's dispatcher is JMP (pointer)). Its own generator,
// so the seeds of the others stay what they were.

namespace tisple_tests::fuzz {

namespace {

std::string Num(int n) {
    return std::to_string(n);
}

struct TablesGen {
    Rng r;

    std::string Global() {
        return "g" + Num(r.Below(4));
    }

    // a u8 value of x (the scrutinee's variable) and the globals
    std::string Value(const std::string& x) {
        switch (r.Below(5)) {
        case 0:
            return x;
        case 1:
            return Global();
        case 2:
            return "(+ " + x + " " + Num(r.Below(256)) + ")";
        case 3:
            return "(at arr (& " + x + " 15))";
        default:
            return "(^ " + Global() + " " + x + ")";
        }
    }

    // one statement of a clause's body
    std::string Statement(const std::string& x) {
        static const char* const kOps[] = {"+", "-", "^", "|", "&"};
        switch (r.Below(4)) {
        case 0:
            return "(set (at arr (& " + Value(x) + " 15)) " + Value(x) + ")";
        case 1:
            return "(set w (+ w (as u16 " + Value(x) + ")))";
        default: {
            std::string g = Global();
            return "(set " + g + " (" + kOps[r.Below(5)] + " " + g + " " + Value(x) + "))";
        }
        }
    }

    std::string Body(const std::string& x) {
        std::string text;
        int n = 1 + r.Below(2);
        for (int i = 0; i < n; i++) {
            text += " " + Statement(x);
        }
        return text;
    }

    // a match on s (an expression of x): dense from a random start, sparse now and then
    std::string Match(const std::string& s, const std::string& x) {
        bool dense = r.Chance(85);
        int clauses = 5 + r.Below(20);
        int value = r.Chance(50) ? 0 : r.Below(60);
        std::string text = "(match " + s;
        for (int c = 0; c < clauses && value < 256; c++) {
            text += "\n        (" + Num(value) + Body(x) + ")";
            value += dense ? 1 : 1 + r.Below(30);
        }
        return text + "\n        (_" + Body(x) + "))";
    }

    std::string EnumMatch(const std::string& x) {
        std::string text = "(match (at EV (& " + x + " 7))";
        for (int m = 0; m < 6; m++) {
            text += "\n        (E.e" + Num(m) + Body(x) + ")";
        }
        return text + ")";
    }

    // fk: a match on its parameter or on a value of it, maybe in a loop whose counter is x
    std::string Function(int k) {
        std::string name = "f" + Num(k);
        std::string text = "(fn " + name + " ((x u8)) u8\n";
        switch (r.Below(4)) {
        case 0:
            text += "    (for i u8 0 " + Num(1 + r.Below(6)) + "\n        " +
                    Match("(+ x i)", "i") + ")\n";
            break;
        case 1:
            text += "    " + EnumMatch("x") + "\n";
            break;
        default:
            text += "    " + Match(r.Chance(50) ? "x" : "(& x " + Num(7 + r.Below(25)) + ")", "x") +
                    "\n";
            break;
        }
        return text + "    (+ " + Global() + " (at arr (& x 15))))\n";
    }

    std::string Program() {
        std::string text = "(enum E (e0 e1 e2 e3 e4 e5))\n";
        text += "(const EV (array E 8) (make (array E 8) E.e0 E.e1 E.e2 E.e3 E.e4 E.e5 E.e" +
                Num(r.Below(6)) + " E.e" + Num(r.Below(6)) + "))\n";
        for (int g = 0; g < 4; g++) {
            text += "(let g" + Num(g) + " u8 " + Num(r.Below(256)) + ")\n";
        }
        text += "(let w u16 0)\n(let arr (array u8 16))\n";
        int count = 2 + r.Below(3);
        for (int k = 0; k < count; k++) {
            text += Function(k);
        }
        text += "(const FS (array (fn (u8) u8) 4) (make (array (fn (u8) u8) 4)";
        for (int k = 0; k < 4; k++) {
            text += " f" + Num(r.Below(count));
        }
        text += "))\n(fn main () u16\n";
        text += "    (for n u8 0 " + Num(10 + r.Below(40)) + "\n";
        text += "        (let v u8 (* n " + Num(1 + r.Below(7)) + "))\n";
        for (int k = 0; k < count; k++) {
            text += "        (set w (+ w (as u16 (f" + Num(k) + " v))))\n";
        }
        text += "        (let h (fn (u8) u8) (at FS (& n 3)))\n";
        text += "        (set w (+ w (as u16 (h (+ v " + Num(r.Below(256)) + "))))))\n";
        return text + "    (+ w (as u16 (+ g0 (+ g1 (+ g2 g3))))))\n";
    }
};

} // namespace

} // namespace tisple_tests::fuzz

namespace tisple_tests {

std::string GenerateTablesProgram(std::uint64_t seed) {
    fuzz::TablesGen g;
    g.r.s = seed * 0x2545F4914F6CDD1Dull + 0x7154;
    if (g.r.Below(100) >= kTablesPercent) {
        return "";
    }
    g.r.Next();
    return g.Program();
}

} // namespace tisple_tests
