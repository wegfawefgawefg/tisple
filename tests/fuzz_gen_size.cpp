#include "fuzz_gen_internal.hpp"

// What the code size budget (backend/size_budget.hpp) turns down or keeps, and the two
// miscompiles of the round that brought it: small helpers with loops called at many places
// with constant counts, heavy small helpers (divisions by constants), unrollable loops with
// bodies that fold per copy, nested ones, ones with continue and break, inside hot loops and
// not; (& (f) 0), (* (f) 0) and (| (f) 255) around calls with effects, through a pointer too
// (constant folding must keep the call); indexes (+ K (as u16 (+ x c))) and (- x c) under a
// guard, read and written (the constant K must stay).

namespace tisple_tests::fuzz {

namespace {

std::string Num(int n) {
    return std::to_string(n);
}

} // namespace

std::string Gen::SizeHelpers() {
    Rng& r = size_rng;
    std::string text = "(let zg u8 " + Num(r.Below(256)) + ")\n(let zh u16 " +
                       Num(static_cast<int>(r.Next() % 65536)) + ")\n";
    text += "(let za (array u8 600))\n";
    text += "(fn zbump () u8 (set zg (+ zg 1)) zg)\n";
    text += "(fn zrnd () u8 (set zh (^ zh (<< zh 7))) (set zh (^ zh (>> zh 9)))\n"
            "    (set zh (^ zh (<< zh 8))) (as u8 zh))\n";
    // a small function with a loop, its count a parameter: constant at most calls
    text += "(fn znoise ((base u16) (count u8)) void\n    (for i u8 0 count (set zh (+ zh base))"
            " (set zg (+ zg (zrnd)))))\n";
    // few nodes, many bytes
    text += "(fn zdiv ((x u16)) u16 (+ (/ x " + Num(3 + r.Below(12)) + ") (% x " +
            Num(5 + r.Below(20)) + ")))\n";
    text += "(fn zrow ((y u8) (k u8)) u8 (at za (+ (* (as u16 (& y 127)) 3) (as u16 (& k 127)))))\n";
    return text;
}

// a call with effects, or one through a pointer (a let in lets)
std::string Gen::SizeCall(std::string& lets) {
    switch (size_rng.Below(4)) {
    case 0:
        return "(zbump)";
    case 1:
        return "(zrnd)";
    default: {
        std::string fp = Fresh("zp");
        lets += "(let " + fp + " (fn () u8) " + (size_rng.Chance(50) ? "zbump" : "zrnd") + ") ";
        return "(" + fp + ")";
    }
    }
}

// (& (f) 0) and its kin: a value whatever f gives, f's effects kept
std::string Gen::SizeAbsorbed() {
    std::string lets;
    std::string call = SizeCall(lets);
    bool word = size_rng.Chance(30);
    std::string x = word ? "(as u16 " + call + ")" : call;
    if (size_rng.Chance(30)) {
        x = "(+ " + x + " " + Num(1 + size_rng.Below(9)) + ")";
    }
    std::string all = word ? "65535" : "255";
    std::string value;
    switch (size_rng.Below(5)) {
    case 0:
        value = "(& " + x + " 0)";
        break;
    case 1:
        value = "(& 0 " + x + ")";
        break;
    case 2:
        value = "(* " + x + " 0)";
        break;
    case 3:
        value = "(| " + all + " " + x + ")";
        break;
    default:
        value = "(| " + x + " " + all + ")";
        break;
    }
    std::vector<const VarInfo*> vars = VarsOf(word ? T::U16 : T::U8, true);
    std::string g =
        vars[static_cast<std::size_t>(size_rng.Below(static_cast<int>(vars.size())))]->name;
    return "(do " + lets + "(set " + g + " (^ " + g + " " + value + ")))";
}

// an index of za as a constant plus a widened byte sum: read or written, in a loop
std::string Gen::SizeIndex() {
    Rng& r = size_rng;
    std::string x = Fresh("zx");
    int from = r.Below(8);
    int to = from + 1 + r.Below(40);
    int k = r.Below(500);
    int c = r.Below(20);
    std::string index = "(+ " + Num(k) + " (as u16 (+ " + x + " " + Num(c) + ")))";
    std::string guard;
    if (r.Chance(30)) {
        // (- x d) under (> x d - 1), as the variant that read t+65534
        int d = 1 + r.Below(3);
        index = "(+ " + Num(k + 4) + " (as u16 (- " + x + " " + Num(d) + ")))";
        guard = "(> " + x + " " + Num(d - 1) + ")";
    } else if (r.Chance(30)) {
        index = "(- (+ " + Num(k + 9) + " (as u16 (+ " + x + " " + Num(c) + "))) " +
                Num(r.Below(9)) + ")";
    }
    std::string at = "(at za " + index + ")";
    std::string body = r.Chance(50) ? "(set zh (+ zh (as u16 " + at + ")))"
                                    : "(set " + at + " (+ (as u8 " + x + ") zg))";
    if (!guard.empty()) {
        body = "(if " + guard + " " + body + ")";
    }
    return "(for " + x + " u8 " + Num(from) + " " + Num(to) + " " + body + ")";
}

// an unrollable loop: its counter read (copies fold it) or not, continue and break in it,
// another inside it, a hot loop around it
std::string Gen::SizeUnroll() {
    Rng& r = size_rng;
    std::string k = Fresh("zk");
    int n = 2 + r.Below(15);
    std::string body;
    switch (r.Below(6)) {
    case 0:
        body = "(set zh (+ zh (zdiv (+ zh (as u16 " + k + ")))))";
        break;
    case 1:
        body = "(set (at za (+ (* (as u16 " + k + ") " + Num(1 + r.Below(30)) + ") " +
               Num(r.Below(100)) + ")) (+ zg " + k + "))";
        break;
    case 2:
        body = "(if (= (& (+ " + k + " zg) 3) 0) (continue)) (set zg (+ zg (zrow " + k + " zg)))";
        break;
    case 3:
        body = "(if (> (+ zg " + k + ") " + Num(200 + r.Below(56)) + ") (break)) (set zg (+ zg 3))";
        break;
    case 4: {
        std::string j = Fresh("zj");
        body = "(for " + j + " u8 0 " + Num(2 + r.Below(5)) + " (set (at za (+ (* (as u16 " + k +
               ") 16) (as u16 " + j + "))) (+ " + k + " " + j + ")))";
        break;
    }
    default:
        body = "(set zg (+ zg (zbump))) (set zh (^ zh (<< zh 1)))";
        break;
    }
    std::string loop = "(for " + k + " u8 0 " + Num(n) + " " + body + ")";
    if (r.Chance(25)) {
        std::string o = Fresh("zo");
        loop = "(for " + o + " u16 0 " + Num(20 + r.Below(200)) + " " + loop + ")";
    }
    return loop;
}

std::string Gen::SizeShape(int depth) {
    Rng& r = size_rng;
    switch (r.Below(7)) {
    case 0:
    case 1:
        return SizeAbsorbed();
    case 2:
        return SizeIndex();
    case 3:
        return "(znoise " + Num(r.Below(3000)) + " " +
               (r.Chance(80) ? Num(r.Below(20)) : "(& " + Var(T::U8) + " 15)") + ")";
    case 4:
        return "(set zh (+ zh (zdiv " + Expr(T::U16, 1) + ")))";
    case 5:
        if (depth > 0 && r.Chance(30)) {
            return "(if " + BoolExpr(1) + " " + SizeUnroll() + " " + Arm(depth - 1, 1, false) + ")";
        }
        return SizeUnroll();
    default:
        return "(set zg (+ zg (zrow " + Expr(T::U8, 1) + " " + Expr(T::U8, 1) + ")))";
    }
}

} // namespace tisple_tests::fuzz
