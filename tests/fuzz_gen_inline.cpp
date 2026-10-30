#include "fuzz_gen_internal.hpp"

// what inlining at the mid level (src/opt/inline.cpp) and the division rules (ranges_divs.cpp)
// change, and what they must leave: small helpers with early returns in either arm, one that
// sets its parameter, one that returns from a loop (never copied), void ones returning from an
// if; calls as statements, as (set x (f ...)) of a global the callee writes, as a let's value,
// inside expressions, as arguments of each other, with arguments that read what the callee
// changes, in loops and in a while's condition; divisions by 0, 1, (| x 1) of the dividend,
// by the dividend itself, and of a remainder by its divisor, signed and unsigned.

namespace tisple_tests::fuzz {

namespace {

const T kInlineTypes[] = {T::U8, T::I8, T::U16, T::I16};

std::string Num(int n) {
    return std::to_string(n);
}

} // namespace

std::string Gen::InlineHelpers() {
    Rng& r = inline_rng;
    std::string text = "(let ig u8 " + Num(r.Below(256)) + ")\n(let ih u16 " +
                       Num(static_cast<int>(r.Next() % 65536)) + ")\n";
    // an early return, in the then arm or the else; its parameter set now and then
    std::string early = "(> x " + Num(r.Below(256)) + ")";
    std::string ret = "(return (+ y ig))";
    text += "(fn ia ((x u8) (y u8)) u8\n";
    text += r.Chance(50) ? "    (if " + early + " " + ret + ")\n"
                         : "    (if " + early + " (set ig (+ ig 1)) " + ret + ")\n";
    if (r.Chance(40)) {
        text += "    (set x (+ x " + Num(1 + r.Below(9)) + "))\n";
    }
    text += "    (set ig (^ ig x))\n    (+ x (* y " + Num(r.Below(5)) + ")))\n";
    // void, returning from an if with an else, a loop of its own
    text += "(fn ib ((p u16)) void\n    (set ih (+ ih p))\n";
    text += "    (if (= (& p 3) " + Num(r.Below(4)) + ") (return) (set ig (+ ig 1)))\n";
    text += "    (for i u8 0 (& ig 3) (set ih (+ ih (as u16 i))))\n";
    if (r.Chance(50)) {
        text += "    (if (> ih " + Num(static_cast<int>(r.Next() % 65536)) + ") (do (set ig 7) (return)))\n";
    }
    text += "    (set ig (- ig 2)))\n";
    // tiny: reads what its callers just set
    text += "(fn ic ((x u8)) u8 (+ x ig))\n";
    // nested calls and divisions by (| b 1)
    text += "(fn id ((a u8) (b u16)) u16\n    (let q u16 (/ b (| b 1)))\n";
    text += "    (if (= a " + Num(r.Below(4)) + ") (return (% b (| b 1))))\n";
    text += "    (+ q (as u16 (ia a (ic a)))))\n";
    // a return from inside a loop: stays a function
    text += "(fn ie ((x u8)) u8\n    (for i u8 0 " + Num(2 + r.Below(6)) +
            " (if (= (& (+ i x) 3) 0) (return i)))\n    (set ig (+ ig x))\n    x)\n";
    return text;
}

T Gen::InlineInt() {
    return kInlineTypes[inline_rng.Below(4)];
}

// x / (| x 1) and its kin, on a variable of type t
std::string Gen::InlineDivision() {
    T t = InlineInt();
    std::vector<const VarInfo*> vars = VarsOf(t, true);
    std::string g = vars[static_cast<std::size_t>(inline_rng.Below(static_cast<int>(vars.size())))]->name;
    std::string v = Var(t);
    std::string w = Var(t);
    std::string value;
    switch (inline_rng.Below(9)) {
    case 0:
        value = "(/ " + v + " (| " + v + " 1))";
        break;
    case 1:
        value = "(% " + v + " (| " + v + " 1))";
        break;
    case 2:
        value = "(/ " + v + " 1)";
        break;
    case 3:
        value = "(% " + v + " 1)";
        break;
    case 4:
        value = "(/ " + v + " (& " + w + " 0))";
        break;
    case 5:
        value = "(% " + v + " (& " + w + " 0))";
        break;
    case 6:
        value = "(/ (% " + v + " (| " + w + " 1)) (| " + w + " 1))";
        break;
    case 7:
        value = "(/ " + v + " " + v + ")";
        break;
    default:
        value = "(% (& " + v + " 7) (| (& " + w + " 15) 8))";
        break;
    }
    return "(set " + g + " " + value + ")";
}

std::string Gen::InlineShape(int depth) {
    Rng& r = inline_rng;
    std::vector<const VarInfo*> bytes = VarsOf(T::U8, true);
    std::string g = bytes[static_cast<std::size_t>(r.Below(static_cast<int>(bytes.size())))]->name;
    std::string dest = r.Chance(30) ? "ig" : g;
    switch (r.Below(10)) {
    case 0:
        return "(ib " + Expr(T::U16, 1) + ")";
    case 1:
        return "(set " + dest + " (ia " + Expr(T::U8, 1) + " " + Expr(T::U8, 1) + "))";
    case 2: {
        std::string l = Fresh("l");
        return "(do (let " + l + " u8 (ic " + Expr(T::U8, 1) + ")) (set " + g + " (+ " + l +
               " ig)))";
    }
    case 3:
        return "(set " + g + " (+ (ia " + Var(T::U8) + " ig) (ic " + Expr(T::U8, 1) + ")))";
    case 4:
        return "(set " + dest + " (ia (ic " + Var(T::U8) + ") (ia ig " + Var(T::U8) + ")))";
    case 5:
        return "(set " + dest + " (ia ig (+ ig " + Num(r.Below(4)) + ")))";
    case 6: {
        std::string k = Fresh("k");
        return "(for " + k + " u8 0 " + Num(1 + r.Below(5)) + " (set ih (+ ih (id " + k + " " +
               Expr(T::U16, 1) + "))) (set " + g + " (ie " + k + ")))";
    }
    case 7:
        return "(while (< (ic " + g + ") " + Num(r.Below(256)) + ") (set " + g + " (+ " + g +
               " 1)))";
    case 8:
        if (depth > 0) {
            return "(if " + BoolExpr(1) + " (ib ih) " + Arm(depth - 1, 1, false) + ")";
        }
        return InlineDivision();
    default:
        return InlineDivision();
    }
}

} // namespace tisple_tests::fuzz
