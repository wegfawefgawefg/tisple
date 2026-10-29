#include "fuzz_gen_internal.hpp"

// what common subexpressions, facts from conditions and compile-time evaluation (src/opt/
// cse.cpp, consts_facts.cpp, eval.cpp) change, and what they must leave: the same work twice
// with nothing between, or a set of an input, a call, a store through a pointer, I/O, an arm
// or a loop that may change one; a division and its remainder apart; ifs, matches, ands and
// whiles on (= v k) and its kin, with arms that set v again or conditions with calls in them;
// calls of small pure functions (and of ones that only look pure) with constant arguments.

namespace tisple_tests::fuzz {

namespace {

const T kCommonTypes[] = {T::U8, T::I8, T::U16, T::I16};

std::string As(T to, T from, const std::string& x) {
    return to == from ? x : "(as " + std::string(Name(to)) + " " + x + ")";
}

} // namespace

// the helpers common shapes call: a loop over constants, a const table read, and one whose
// while ends (its count stays even) though the passes cannot tell
std::string Gen::CommonHelpers() {
    std::string mul = std::to_string(3 + 2 * common_rng.Below(4));
    return "(fn cq ((v u8) (w u16)) u16 (let s u16 w)\n"
           "    (for i u8 0 (& v 15) (set s (+ (* s " +
           mul +
           ") (as u16 (^ i v))))) s)\n"
           "(fn ck ((v u8)) u16 (+ (at KT (& v 7)) (as u16 v)))\n"
           "(fn cw ((v u8)) u8 (let c u8 (& v 254)) (while (!= c 0) (set c (- c 2))) (+ c v))\n";
}

T Gen::CommonInt() {
    return kCommonTypes[common_rng.Below(4)];
}

// a writable variable of type t
std::string Gen::CommonVar(T t) {
    std::vector<const VarInfo*> vars = VarsOf(t, true);
    return vars[static_cast<std::size_t>(common_rng.Below(static_cast<int>(vars.size())))]->name;
}

// worth keeping in a local: a product, a quotient or remainder, a shift by a variable, a call
std::string Gen::CommonWork(T t, const std::string& a, const std::string& b) {
    switch (common_rng.Below(7)) {
    case 0:
        return "(* " + a + " " + b + ")";
    case 1:
        return "(/ " + a + " " + b + ")";
    case 2:
        return "(% " + a + " " + b + ")";
    case 3:
        return "(<< " + a + " (& " + NonLit(T::U8, 0) + " 7))";
    case 4:
        // a, not b: b may be a literal, which a constant divisor gives no type
        return "(+ (* " + a + " " + b + ") (/ " + a + " " + Divisor(t) + "))";
    case 5:
        // a literal b takes its type from where it is used: none to convert from here
        return As(t, T::U16, "(cq " + As(T::U8, t, a) + " " + As(T::U16, t, Var(t)) + ")");
    default:
        return "(/ " + a + " " + Divisor(t) + ")";
    }
}

// what comes between two workings out: nothing, or something that may change an input
std::string Gen::CommonBetween(T t, const std::string& a, int depth) {
    switch (common_rng.Below(10)) {
    case 0:
        return " (set " + a + " " + Expr(t, 1) + ")";
    case 1:
        if (current > 0) {
            return " " +
                   CallText(fns[static_cast<std::size_t>(rng.Below(static_cast<int>(current)))], 1);
        }
        return "";
    case 2: {
        std::string store = Store();
        return store.empty() ? "" : " " + store;
    }
    case 3:
        return " (set " + CommonVar(T::U8) + " (io_read u8 " + IoAddress() + "))";
    case 4:
        return " (if " + BoolExpr(1) + " (set " + a + " " + Expr(t, 1) + "))";
    case 5: {
        if (depth <= 0) {
            return "";
        }
        scopes.emplace_back();
        std::string stmt = Stmt(depth - 1);
        scopes.pop_back();
        DropPointers();
        return " " + stmt;
    }
    case 6:
        return " (if " + BoolExpr(1) + " (set " + CommonVar(t) + " " + Expr(t, 1) + "))";
    default:
        return "";
    }
}

// the same work twice: in a row, in an arm, in a loop's body, in an and's right side
std::string Gen::CommonTwice(int depth) {
    T t = CommonInt();
    std::string a = CommonVar(t);
    std::string b = common_rng.Chance(30)   ? Literal(t)
                    : common_rng.Chance(50) ? Var(t)
                                            : NonLit(t, 1);
    std::string work = CommonWork(t, a, b);
    std::string g = CommonVar(t);
    std::string h = CommonVar(t);
    std::string first = "(set " + g + " (^ " + g + " " + work + "))";
    std::string again = "(set " + h + " (+ " + h + " " + work + "))";
    std::string text = "(do " + first + CommonBetween(t, a, depth);
    switch (common_rng.Below(5)) {
    case 0:
        text += " (if " + BoolExpr(1) + " " + again + ")";
        break;
    case 1: {
        std::string n = Fresh("n");
        text += " (for " + n + " u8 0 " + std::to_string(common_rng.Below(4)) + " " + again;
        if (common_rng.Chance(40)) {
            text += " (set " + a + " " + Expr(t, 1) + ")";
        }
        text += ")";
        break;
    }
    case 2:
        text +=
            " (if (and " + BoolExpr(1) + " (!= " + work + " " + Literal(t) + ")) " + again + ")";
        break;
    default:
        text += " " + again;
        break;
    }
    return text + ")";
}

// a division and its remainder of the same operands, apart
std::string Gen::DivApart(int depth) {
    T t = CommonInt();
    std::string a = CommonVar(t);
    std::string b = common_rng.Chance(40)   ? Divisor(t)
                    : common_rng.Chance(50) ? Var(t)
                                            : NonLit(t, 1);
    bool div_first = common_rng.Chance(50);
    std::string one = std::string(div_first ? "(/ " : "(% ") + a + " " + b + ")";
    std::string two = std::string(div_first ? "(% " : "(/ ") + a + " " + b + ")";
    std::string g = CommonVar(t);
    std::string h = CommonVar(t);
    return "(do (set " + g + " (^ " + g + " " + one + ")) (if " + BoolExpr(1) + " (set " +
           CommonVar(T::U8) + " " + Expr(T::U8, 1) + "))" + CommonBetween(t, a, depth) + " (set " +
           h + " (+ " + h + " " + two + ")))";
}

// a branch on (= v k) or its kin, with arms that read v (or set it, then read it)
std::string Gen::CondFact(int depth) {
    T t = CommonInt();
    std::string v = CommonVar(t);
    std::string k = Literal(t);
    std::string g = CommonVar(t);
    std::string x = Var(t);
    std::string use = "(set " + g + " (^ " + g + " (/ " + x + " " + v + ")))";
    if (common_rng.Chance(30)) {
        use = "(set " + g + " (+ " + g + " (* " + v + " " + x + ")))";
    }
    if (common_rng.Chance(20)) {
        use = "(do (set " + v + " " + Expr(t, 1) + ") " + use + ")";
    }
    std::string other = "(set " + g + " " + v + ")";
    switch (common_rng.Below(8)) {
    case 0:
        return "(if (!= " + v + " " + k + ") " + other + " " + use + ")";
    case 1:
        return "(if (not (!= " + k + " " + v + ")) " + use + ")";
    case 2:
        return "(if (and (= " + v + " " + k + ") " + BoolExpr(1) + ") " + use + " " + other + ")";
    case 3:
        return "(if (or (!= " + v + " " + k + ") " + BoolExpr(1) + ") " + other + " " + use + ")";
    case 4: {
        // a condition with a call in it, which may change v after it is read
        std::string call = Expr(t, 2);
        // v first: it gives the expression its type
        return "(if (and (= " + v + " " + k + ") (= " + v + " " + call + ")) " + use + ")";
    }
    case 5: {
        std::string m = CommonVar(T::U8);
        std::string c1 = std::to_string(common_rng.Below(256));
        std::string c2 = std::to_string((std::stoi(c1) + 1 + common_rng.Below(255)) % 256);
        std::string arm = "(set " + m + " (+ " + m + " (/ " + Var(T::U8) + " (| " + m + " 1))))";
        return "(match " + m + " (" + c1 + " " + arm + ") (" + c2 + " (set " + m + " (* " + m +
               " 3))) (_ (do)))";
    }
    case 6: {
        // after a while that ends normally, its condition was false
        std::string c = Fresh("c");
        std::string end = std::to_string(common_rng.Below(256));
        std::string body =
            depth > 0 && common_rng.Chance(30) ? " (if (= " + c + " 3) (break))" : "";
        return "(do (let " + c + " u8 " + Expr(T::U8, 1) + ") (while (!= " + c + " " + end +
               ") (set " + c + " (+ " + c + " 1))" + body + ") (set " + CommonVar(T::U8) + " (* " +
               c + " 3)))";
    }
    default:
        return "(if (= " + v + " " + k + ") " + use + " " + other + ")";
    }
}

// a call of a helper with constant arguments, or arguments that only become constant
std::string Gen::ConstCall() {
    std::string g = CommonVar(T::U16);
    std::string v = std::to_string(common_rng.Below(256));
    std::string w = std::to_string(common_rng.Below(65536));
    std::string call;
    switch (common_rng.Below(5)) {
    case 0:
        call = "(cq " + v + " " + w + ")";
        break;
    case 1:
        call = "(ck " + v + ")";
        break;
    case 2:
        call = "(as u16 (cw " + v + "))";
        break;
    case 3: {
        std::string k = Fresh("k");
        return "(do (let " + k + " u8 " + v + ") (set " + g + " (+ " + g + " (cq " + k + " " + w +
               "))))";
    }
    default:
        call = "(cq " + Var(T::U8) + " " + w + ")";
        break;
    }
    return "(set " + g + " (^ " + g + " " + call + "))";
}

std::string Gen::CommonShape(int depth) {
    switch (common_rng.Below(8)) {
    case 0:
    case 1:
    case 2:
        return CommonTwice(depth);
    case 3:
        return DivApart(depth);
    case 4:
    case 5:
        return CondFact(depth);
    default:
        return ConstCall();
    }
}

} // namespace tisple_tests::fuzz
