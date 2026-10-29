#include "fuzz_gen_internal.hpp"

// what the round's variable ranges, the ranges sets give, elements no store reaches and the
// slice 5 identities (src/opt/ranges_vars.cpp, ranges.cpp, ranges_elements.cpp,
// collapse_continue.cpp, consts_eval.cpp) change, and what they must leave: locals stepped by
// a variable that holds one of two constants (65535 and 1, 255 and 1), from starts at the
// edges of their types, with compares near where they end; steps a second write, a loop
// inside or a let in another do spoils; (% g k) then (/ g k) with a set, call, loop or arm
// between; helpers called with the same constants nearly everywhere; an array whose elements
// only some literal and masked indexes store, read at others, now and then through a
// pointer; (if c (continue)) items; (+ x (~ x)) and its kin; a bool tested false.

namespace tisple_tests::fuzz {

namespace {

const T kValueTypes[] = {T::U8, T::I8, T::U16, T::I16};

std::string As(T to, T from, const std::string& x) {
    return to == from ? x : "(as " + std::string(Name(to)) + " " + x + ")";
}

long Max(T t) {
    return t == T::U8 ? 255 : t == T::I8 ? 127 : t == T::U16 ? 65535 : 32767;
}

long Min(T t) {
    return t == T::I8 ? -128 : t == T::I16 ? -32768 : 0;
}

} // namespace

T Gen::ValuesInt() {
    return kValueTypes[values_rng.Below(4)];
}

std::string Gen::ValuesVar(T t) {
    std::vector<const VarInfo*> vars = VarsOf(t, true);
    return vars[static_cast<std::size_t>(values_rng.Below(static_cast<int>(vars.size())))]->name;
}

// a constant at or near an edge of t: its ends, 0, 1, all ones, or any
std::string Gen::EdgeLiteral(T t) {
    long lo = Min(t);
    long hi = Max(t);
    switch (values_rng.Below(8)) {
    case 0:
        return std::to_string(lo + values_rng.Below(3));
    case 1:
        return std::to_string(hi - values_rng.Below(3));
    case 2:
        return std::to_string(values_rng.Below(3));
    case 3:
        // all ones: -1 signed, the top unsigned
        return lo < 0 ? "-1" : std::to_string(hi);
    case 4:
        return std::to_string(lo < 0 ? -1 - values_rng.Below(4) : hi / 2 + values_rng.Below(3));
    default:
        return std::to_string(lo + static_cast<long>(values_rng.Next() % (hi - lo + 1)));
    }
}

// helpers the shapes call with the same constants nearly always: vk reads a global, so it is
// not worked out at compile time; vm sets its parameter on one path
std::string Gen::ValuesHelpers() {
    values_a = EdgeLiteral(T::U16);
    values_b = EdgeLiteral(T::I8);
    values_m = EdgeLiteral(T::U8);
    std::string text = "(let vg u16 " + EdgeLiteral(T::U16) + ")\n(let vt (array u8 16)";
    if (values_rng.Chance(60)) {
        text += " (make (array u8 16)";
        for (int k = 0; k < 16; k++) {
            text += " " + std::to_string(values_rng.Chance(50) ? k : values_rng.Below(256));
        }
        text += ")";
    }
    text += ")\n(let vq (array S 4))\n";
    for (int i = 0; i < 2; i++) {
        values_stored[i] = values_rng.Below(16);
    }
    return text + "(fn vk ((a u16) (b i8)) u16 (let r u16 (+ vg a))\n"
                  "    (if (>= a 65535) (set r (- r 1))) (if (< b 0) (set r (^ r 255)))\n"
                  "    (if (> (as i16 b) 100) (set r (+ r (as u16 b)))) r)\n"
                  "(fn vm ((x u8)) u8 (if (> vg 1000) (set x (+ x 1))) (if (= x 255) 7 (* x 3)))\n";
}

std::string Gen::ValuesCall() {
    T t = T::U16;
    std::string g = ValuesVar(t);
    std::string a = values_rng.Chance(85) ? values_a : EdgeLiteral(T::U16);
    std::string b = values_rng.Chance(85) ? values_b : EdgeLiteral(T::I8);
    if (values_rng.Chance(30)) {
        std::string x = values_rng.Chance(80) ? values_m : EdgeLiteral(T::U8);
        return "(set " + ValuesVar(T::U8) + " (vm " + x + "))";
    }
    return "(set " + g + " (^ " + g + " (vk " + a + " " + b + ")))";
}

// a local stepped by a variable holding one of two constants, compared after the step
std::string Gen::StepLoop(int depth) {
    T t = ValuesInt();
    std::string s = Fresh("s");
    std::string v = Fresh("v");
    std::string c = Fresh("c");
    std::string tw = Fresh("tw");
    std::string g = ValuesVar(t);
    std::string type = Name(t);
    std::string n = std::to_string(1 + values_rng.Below(values_rng.Chance(50) ? 6 : 200));
    std::string text = "(do (let " + s + " " + type + " " + EdgeLiteral(t) + ") (if " +
                       BoolExpr(1) + " (set " + s + " " + EdgeLiteral(t) + ")) (let " + v + " " +
                       type + " " + EdgeLiteral(t) + ") ";
    bool is_for = values_rng.Chance(30);
    bool apart = values_rng.Chance(12); // the loop in a do of its own: no step rule
    text += apart ? "(if true (do " : "";
    if (is_for) {
        text += "(for " + c + " u8 0 " + n + " ";
    } else {
        text +=
            "(let " + c + " u8 0) (while (< " + c + " " + n + ") (set " + c + " (+ " + c + " 1)) ";
    }
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{c, T::U8, true});
    if (values_rng.Chance(20)) {
        text += "(if " + BoolExpr(1) + " (continue)) ";
    }
    switch (values_rng.Below(3)) {
    case 0:
        text += "(set " + v + " (+ " + v + " " + s + ")) ";
        break;
    case 1:
        text += "(set " + v + " (+ " + s + " " + v + ")) ";
        break;
    default:
        text += "(set " + v + " (- " + v + " " + s + ")) ";
        break;
    }
    switch (values_rng.Below(10)) {
    case 0:
        // a second step, by a constant
        text += "(set " + v + " (+ " + v + " " + EdgeLiteral(t) + ")) ";
        break;
    case 1:
        // a write that is no step
        text += "(if " + BoolExpr(1) + " (set " + v + " " + EdgeLiteral(t) + ")) ";
        break;
    case 2:
        // a step in a loop inside: more than one a pass
        text += "(for " + Fresh("k") + " u8 0 3 (set " + v + " (+ " + v + " " + s + "))) ";
        break;
    default:
        break;
    }
    std::string look;
    switch (values_rng.Below(5)) {
    case 0:
        look = v;
        break;
    case 1:
        look = "(<< " + v + " 1)";
        break;
    case 2:
        look = "(- " + v + " " + EdgeLiteral(t) + ")";
        break;
    case 3:
        look = "(+ " + v + " " + EdgeLiteral(t) + ")";
        break;
    default:
        look = "(>> " + v + " " + std::to_string(1 + values_rng.Below(3)) + ")";
        break;
    }
    static const char* const kTests[] = {"<", "<=", ">", ">=", "=", "!="};
    text += "(let " + tw + " " + type + " " + look + ") (if (" + kTests[values_rng.Below(6)] + " " +
            tw + " " + EdgeLiteral(t) + ") (set " + g + " (+ " + g + " 1)))";
    if (depth > 0 && values_rng.Chance(20)) {
        text += " " + Stmt(depth - 1);
    }
    scopes.pop_back();
    DropPointers();
    text += apart ? ")))" : ")";
    return text + " (set " + g + " (^ " + g + " " + v + ")))";
}

// (% g k) then (/ g k), a mask then a compare, with something between that may write g
std::string Gen::SetFacts(int depth) {
    T t = ValuesInt();
    std::string g = ValuesVar(t);
    std::string h = ValuesVar(t);
    std::string k = values_rng.Chance(10) ? "0" : std::to_string(1 + values_rng.Below(12));
    std::string between;
    switch (values_rng.Below(7)) {
    case 0:
        between = " (set " + g + " " + Expr(t, 1) + ")";
        break;
    case 1:
        if (current > 0) {
            between =
                " " +
                CallText(fns[static_cast<std::size_t>(rng.Below(static_cast<int>(current)))], 1);
        }
        break;
    case 2:
        between = " (for " + Fresh("j") + " u8 0 2 (set " + g + " (+ " + g + " 1)))";
        break;
    case 3:
        between = " (if " + BoolExpr(1) + " (set " + g + " " + EdgeLiteral(t) + "))";
        break;
    case 4:
        // its lets end with the do
        scopes.emplace_back();
        between = depth > 0 ? " " + Stmt(depth - 1) : "";
        scopes.pop_back();
        DropPointers();
        break;
    default:
        break;
    }
    if (values_rng.Chance(60)) {
        return "(do (set " + g + " (% " + g + " " + k + "))" + between + " (set " + g + " (/ " + g +
               " " + k + ")))";
    }
    std::string mask = std::to_string((1 << (1 + values_rng.Below(6))) - 1);
    static const char* const kTests[] = {"<", "<=", ">", ">=", "=", "!="};
    return "(do (set " + g + " (& " + NonLit(t, 1) + " " + mask + "))" + between + " (if (" +
           kTests[values_rng.Below(6)] + " " + g + " " + EdgeLiteral(t) + ") (set " + h + " " +
           Expr(t, 1) + ") (set " + h + " " + g + ")))";
}

// vt stored at two indexes of the program's and a masked range, read at literal and masked
// indexes; vq's fields likewise
std::string Gen::ElementShape() {
    T t = T::U8;
    std::string g = ValuesVar(t);
    std::string x = NonLit(T::U8, 1);
    switch (values_rng.Below(7)) {
    case 0:
        return "(set (at vt " + std::to_string(values_stored[values_rng.Below(2)]) + ") " +
               Expr(t, 1) + ")";
    case 1:
        return "(set (at vt (+ (& " + x + " 3) 12)) " + Expr(t, 1) + ")";
    case 2:
        // through a pointer now and then: every element may change
        if (values_rng.Chance(25)) {
            return "(store (addr_of (at vt " + std::to_string(values_rng.Below(16)) + ")) " +
                   Expr(t, 1) + ")";
        }
        return "(set (at vq " + std::to_string(values_rng.Below(4)) + " a) " + Expr(t, 1) + ")";
    case 3:
        return "(set " + g + " (+ " + g + " (at vt " + std::to_string(values_rng.Below(16)) + ")))";
    case 4:
        return "(if (< (at vt (& " + x + " 7)) " + std::to_string(values_rng.Below(20)) +
               ") (set " + g + " " + Expr(t, 1) + "))";
    case 5:
        return "(set " + g + " (+ (at vq " + std::to_string(values_rng.Below(4)) + " " +
               (values_rng.Chance(50) ? "a" : "d") + ") " + x + "))";
    default:
        return "(if (at vq (& " + x + " 3) c) (set " + g + " (as u8 (at vq " +
               std::to_string(values_rng.Below(4)) + " b))))";
    }
}

// a loop whose body skips its rest on a condition, once or twice
std::string Gen::ContinueLoop(int depth) {
    std::string n = Fresh("n");
    bool is_for = values_rng.Chance(60);
    std::string head = is_for ? "(for " + n + " u8 0 " + std::to_string(values_rng.Below(6)) + " "
                              : "(let " + n + " u8 0) (while (< " + n + " " +
                                    std::to_string(values_rng.Below(6)) + ") (set " + n + " (+ " +
                                    n + " 1)) ";
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{n, T::U8, true});
    loops++;
    std::string body;
    int count = 2 + values_rng.Below(4);
    for (int i = 0; i < count; i++) {
        T t = ValuesInt();
        switch (values_rng.Below(5)) {
        case 0:
            body += " (if " + BoolExpr(1) + " (continue))";
            break;
        case 1:
            body += " (set " + ValuesVar(t) + " " + Expr(t, 1) + ")";
            break;
        case 2:
            body += " (set " + ValuesVar(t) + " (+ " + ValuesVar(t) + " " + As(t, T::U8, n) + "))";
            break;
        case 3:
            body += depth > 0 ? " " + Stmt(depth - 1) : " (set " + ValuesVar(t) + " 1)";
            break;
        default: {
            std::string g = ValuesVar(t);
            body += " (set " + g + " (+ " + g + " 1))";
            break;
        }
        }
    }
    if (values_rng.Chance(40)) {
        body += " (if " + BoolExpr(1) + " (continue))";
    }
    loops--;
    scopes.pop_back();
    DropPointers();
    return "(do " + head + body + "))";
}

// (+ x (~ x)) and its kin, a bool tested false, an enum compared
std::string Gen::Complements() {
    T t = ValuesInt();
    std::string g = ValuesVar(t);
    std::string x = values_rng.Chance(70) ? ValuesVar(t) : NonLit(t, 1);
    static const char* const kOps[] = {"+", "|", "^", "&"};
    switch (values_rng.Below(4)) {
    case 0:
    case 1: {
        std::string op = kOps[values_rng.Below(4)];
        return values_rng.Chance(50) ? "(set " + g + " (" + op + " " + x + " (~ " + x + ")))"
                                     : "(set " + g + " (" + op + " (~ " + x + ") " + x + "))";
    }
    case 2: {
        std::string b = ValuesVar(T::Bool);
        return "(if " + b + " (set " + g + " " + EdgeLiteral(t) + ") (set " + g + " (if " + b +
               " " + EdgeLiteral(t) + " " + Expr(t, 1) + ")))";
    }
    default:
        return "(if (" + std::string(values_rng.Chance(50) ? "=" : "!=") + " " +
               ValuesVar(T::Enum) + " E.e" + std::to_string(values_rng.Below(kEnumMembers)) +
               ") (set " + g + " " + Expr(t, 1) + "))";
    }
}

std::string Gen::ValuesShape(int depth) {
    switch (values_rng.Below(10)) {
    case 0:
    case 1:
    case 2:
        return StepLoop(depth);
    case 3:
        return SetFacts(depth);
    case 4:
        return ValuesCall();
    case 5:
    case 6:
        return ElementShape();
    case 7:
        return ContinueLoop(depth);
    default:
        return Complements();
    }
}

} // namespace tisple_tests::fuzz
