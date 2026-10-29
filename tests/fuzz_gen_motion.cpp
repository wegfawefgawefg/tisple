#include "fuzz_gen_internal.hpp"

// what constant propagation and loop-invariant code motion (src/opt/consts.cpp, licm.cpp)
// change, and what they must leave: divisions, products and shifts of values a loop never
// changes, next to the same with an input the loop sets, a call in it sets, a store through a
// pointer may reach, a load, or the counter; loops that run zero times, once, or leave early;
// lets of constants read after branches, matches and loops that may or may not change them,
// globals set before calls that may or may not write them; (- x x) and its kin.

namespace tisple_tests::fuzz {

namespace {

const T kMotionTypes[] = {T::U8, T::I8, T::U16, T::I16};

std::string AsU8(T t, const std::string& x) {
    return t == T::U8 ? x : "(as u8 " + x + ")";
}

} // namespace

T Gen::MotionInt() {
    return kMotionTypes[motion_rng.Below(4)];
}

// an operand: a variable, a literal (when one may stand there), an array read, an expression
// with calls in it
std::string Gen::MotionOperand(T t, bool literal) {
    switch (motion_rng.Below(5)) {
    case 0:
    case 1:
        return Var(t);
    case 2:
        return literal ? Literal(t) : Var(t);
    default:
        return NonLit(t, 1);
    }
}

// work worth moving out of a loop, of type t, on operands a and b
std::string Gen::MotionWork(T t, const std::string& a, const std::string& b) {
    switch (motion_rng.Below(7)) {
    case 0:
        return "(/ " + a + " " + b + ")";
    case 1:
        return "(% " + a + " " + b + ")";
    case 2:
        return "(/ " + a + " " + Divisor(t) + ")";
    case 3:
        return "(* " + a + " " + b + ")";
    case 4:
        return "(<< " + a + " (& " + NonLit(T::U8, 0) + " 15))";
    case 5:
        return "(+ (* " + a + " " + b + ") (/ " + b + " " + Divisor(t) + "))";
    default: {
        std::string read = "(at sm (& " + AsU8(t, "(% " + a + " " + b + ")") + " 31))";
        return t == T::U8 ? read : "(as " + std::string(Name(t)) + " " + read + ")";
    }
    }
}

// a statement of an invariant loop's body: the work folded into a global, now and then a
// write that spoils it, an arm, an early exit
std::string Gen::MotionItem(T t, const std::string& work, const std::string& count_var, int depth) {
    std::string g = DeadTarget(t);
    std::string fold = "(set " + g + " (^ " + g + " " + work + "))";
    switch (motion_rng.Below(10)) {
    case 0:
        // in an arm: it runs on some passes only
        return "(if " + BoolExpr(1) + " " + fold + ")";
    case 1:
        // after a test that may leave the loop
        return "(do (if " + BoolExpr(1) + " (break)) " + fold + ")";
    case 2:
        // an input changes on every pass, or on some
        return "(do " + fold + " (set " + DeadTarget(t) + " " + Expr(t, 1) + "))";
    case 3: {
        // a call that may write what the work reads
        if (current > 0) {
            return "(do " +
                   CallText(fns[static_cast<std::size_t>(rng.Below(static_cast<int>(current)))],
                            1) +
                   " " + fold + ")";
        }
        return fold;
    }
    case 4: {
        // a store through a pointer into an array the work may read
        std::string store = Store();
        return store.empty() ? fold : "(do " + store + " " + fold + ")";
    }
    case 5:
        // the counter in the work
        if (!count_var.empty() && t == T::U8) {
            return "(set " + g + " (^ " + g + " (/ " + work + " (| " + count_var + " 1))))";
        }
        return fold;
    case 6: {
        if (depth <= 0) {
            return fold;
        }
        // its lets end with the do
        scopes.emplace_back();
        std::string stmt = Stmt(depth - 1);
        scopes.pop_back();
        DropPointers();
        return "(do " + stmt + " " + fold + ")";
    }
    default:
        return fold;
    }
}

// a for or while whose body works out something it never changes the inputs of (most of the
// time); ranges and bounds that may be empty
std::string Gen::InvariantLoop(int depth) {
    T t = MotionInt();
    std::string a = MotionOperand(t, false);
    std::string b = MotionOperand(t, true);
    std::string work = MotionWork(t, a, b);
    std::string n = Fresh("n");
    T count_type = T::U8;
    std::string head;
    std::string lets;
    switch (motion_rng.Below(6)) {
    case 0:
        head = "(for " + n + " u8 0 " + std::to_string(motion_rng.Below(6)) + " ";
        break;
    case 1:
        head = "(for " + n + " u8 (& " + NonLit(T::U8, 1) + " 7) (& " + NonLit(T::U8, 1) + " 7) ";
        break;
    case 2:
        count_type = T::I8;
        head = "(for " + n + " i8 (as i8 (- (& " + NonLit(T::U8, 1) + " 7) 4)) 3 ";
        break;
    case 3:
        count_type = T::U16;
        head = "(for " + n + " u16 (as u16 (& " + NonLit(T::U8, 1) + " 3)) 3 ";
        break;
    case 4: {
        // (let c 0) (while (< c k) ...), k a literal or a masked variable
        std::string k = motion_rng.Chance(50) ? std::to_string(motion_rng.Below(5))
                                              : "(& " + NonLit(T::U8, 1) + " 3)";
        lets = "(let " + n + " u8 0) ";
        head = "(while (< " + n + " " + k + ") (set " + n + " (+ " + n + " 1)) ";
        break;
    }
    default:
        // the work in the condition too
        lets = "(let " + n + " u8 0) ";
        head = "(while (and (< " + n + " 3) (!= " + AsU8(t, work) + " 7)) (set " + n + " (+ " + n +
               " 1)) ";
        break;
    }
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{n, count_type, true});
    bool was_while = in_while;
    in_while = head[1] == 'w'; // no continue in a while: it could skip the step
    loops++;
    std::string body;
    int count = 1 + motion_rng.Below(3);
    for (int i = 0; i < count; i++) {
        body += " " + MotionItem(t, work, head[1] == 'f' && count_type == T::U8 ? n : "", depth);
    }
    if (depth > 0 && motion_rng.Chance(25)) {
        // a loop inside, which may run zero times, with the same work in it
        body += " " + InvariantLoop(depth - 1);
    }
    loops--;
    in_while = was_while;
    scopes.pop_back();
    DropPointers();
    return "(do " + lets + head + body + "))";
}

// a let of a constant, branches and loops that may change it, then reads of it
std::string Gen::ConstLet(int depth) {
    T t = MotionInt();
    std::string k = Fresh("k");
    std::string text = "(do (let " + k + " " + Name(t) + " " + Literal(t) + ")";
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{k, t, false});
    std::string set = "(set " + k + " " + Literal(t) + ")";
    switch (motion_rng.Below(7)) {
    case 0:
        text += " (if " + BoolExpr(1) + " " + set + ")";
        break;
    case 1:
        text +=
            " (if (= " + k + " " + Literal(t) + ") " + set + " (set " + k + " (+ " + k + " 1)))";
        break;
    case 2:
        text += " (match " + AsU8(t, k) + " (0 " + set + ") (1) (" +
                std::to_string(2 + motion_rng.Below(254)) + " (set " + k + " " + Expr(t, 1) +
                ")) (_ (do)))";
        break;
    case 3: {
        std::string c = Fresh("c");
        text += " (let " + c + " u8 0) (while (< " + c + " " + std::to_string(motion_rng.Below(4)) +
                ") (set " + c + " (+ " + c + " 1)) (if (= " + c + " 2) " + set + "))";
        break;
    }
    case 4:
        text += " (while (< " + AsU8(t, k) + " 0) " + set + ")"; // never runs
        break;
    case 5:
        if (depth > 0) {
            text += " " + Stmt(depth - 1);
        }
        break;
    default:
        break;
    }
    std::string g = DeadTarget(t);
    switch (motion_rng.Below(4)) {
    case 0:
        text += " (set " + g + " (/ " + MotionOperand(t, true) + " " + k + "))"; // maybe by zero
        break;
    case 1:
        text += " (if (< " + k + " " + Literal(t) + ") (set " + g + " " + Expr(t, 1) + ") (set " +
                g + " (% " + g + " " + k + ")))";
        break;
    case 2:
        text += " (set " + g + " (+ " + g + " (* " + k + " " + MotionOperand(t, true) + ")))";
        break;
    default:
        text += " (set " + g + " (^ " + g + " " + k + "))";
        break;
    }
    scopes.pop_back();
    DropPointers();
    return text + ")";
}

// a global set to a constant, something that may or may not change it, then a read
std::string Gen::ConstGlobal() {
    T t = MotionInt();
    std::string g = DeadTarget(t);
    std::string h = DeadTarget(t);
    std::string text = "(do (set " + g + " " + Literal(t) + ")";
    switch (motion_rng.Below(4)) {
    case 0:
        if (current > 0) {
            text +=
                " " +
                CallText(fns[static_cast<std::size_t>(rng.Below(static_cast<int>(current)))], 1);
        }
        break;
    case 1: {
        std::string store = Store();
        text += store.empty() ? "" : " " + store;
        break;
    }
    case 2:
        text += " (set " + h + " " + Expr(t, 1) + ")";
        break;
    default:
        break;
    }
    return text + " (set " + h + " (+ " + h + " (/ " + g + " " + Divisor(t) + "))))";
}

// (- x x), (^ x x), (% x x), (| x x), (& x x), (= x x): x pure or not, a load or not
std::string Gen::SameOperands() {
    T t = MotionInt();
    std::string x = MotionOperand(t, false);
    std::string g = DeadTarget(t);
    static const char* const kOps[] = {"-", "^", "%", "|", "&", "/"};
    if (motion_rng.Chance(20)) {
        static const char* const kTests[] = {"=", "!=", "<", "<=", ">", ">="};
        return "(if (" + std::string(kTests[motion_rng.Below(6)]) + " " + x + " " + x + ") (set " +
               g + " (+ " + g + " 1)))";
    }
    return "(set " + g + " (^ " + g + " (" + kOps[motion_rng.Below(6)] + " " + x + " " + x + ")))";
}

std::string Gen::MotionShape(int depth) {
    switch (motion_rng.Below(8)) {
    case 0:
    case 1:
    case 2:
        return InvariantLoop(depth);
    case 3:
    case 4:
        return ConstLet(depth);
    case 5:
        return ConstGlobal();
    default:
        return SameOperands();
    }
}

} // namespace tisple_tests::fuzz
