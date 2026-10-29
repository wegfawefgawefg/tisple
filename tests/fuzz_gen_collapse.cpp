#include "fuzz_gen_internal.hpp"

// what loop collapse, value ranges and unread arguments (src/opt/collapse.cpp, ranges.cpp,
// args.cpp) change, and what they must leave: fors and counting whiles whose bodies set
// globals and elements from what no pass changes, add to accumulators, read the counter for
// the last pass to decide, next to bodies that read what an earlier pass wrote, write under a
// branch on the counter or at an index by it, store through pointers, call, or leave early;
// compares of masks, remainders and counters against literals at the edges of their ranges,
// matches on counters, ands and ors with a literal on the right.

namespace tisple_tests::fuzz {

namespace {

const T kCollapseTypes[] = {T::U8, T::I8, T::U16, T::I16};

std::string As(T to, T from, const std::string& x) {
    return to == from ? x : "(as " + std::string(Name(to)) + " " + x + ")";
}

} // namespace

T Gen::CollapseInt() {
    return kCollapseTypes[collapse_rng.Below(4)];
}

std::string Gen::CollapseVar(T t) {
    std::vector<const VarInfo*> vars = VarsOf(t, true);
    return vars[static_cast<std::size_t>(collapse_rng.Below(static_cast<int>(vars.size())))]->name;
}

// a statement of a loop body: most of the time the same on every pass, or the last pass's
std::string Gen::CollapseItem(const std::string& count, T count_type, bool is_for, int depth) {
    T t = CollapseInt();
    std::string g = CollapseVar(t);
    static const char* const kOps[] = {"+", "-", "^", "|", "&"};
    switch (collapse_rng.Below(14)) {
    case 0:
    case 1:
        return "(set " + g + " " + Expr(t, 1) + ")";
    case 2:
    case 3: {
        std::string k = collapse_rng.Chance(40)   ? Literal(t)
                        : collapse_rng.Chance(50) ? Var(t)
                                                  : NonLit(t, 1);
        std::string op = kOps[collapse_rng.Below(5)];
        // k first now and then: (+ k g) adds the same
        if (op != "-" && collapse_rng.Chance(30) && k[0] == '(') {
            return "(set " + g + " (" + op + " " + k + " " + g + "))";
        }
        return "(set " + g + " (" + op + " " + g + " " + k + "))";
    }
    case 4:
        return "(set " + g + " (^ " + As(t, count_type, count) + " " + Literal(t) + "))";
    case 5: {
        std::vector<const ArrayInfo*> arrs = ArraysOf(t, true);
        if (arrs.empty()) {
            return "(set " + g + " " + Expr(t, 1) + ")";
        }
        const ArrayInfo& a =
            *arrs[static_cast<std::size_t>(collapse_rng.Below(static_cast<int>(arrs.size())))];
        std::string index = collapse_rng.Chance(60)
                                ? std::to_string(collapse_rng.Below(static_cast<int>(a.count)))
                                : Index(a);
        return "(set (at " + a.name + " " + index + ") " + Expr(t, 1) + ")";
    }
    case 6: {
        std::string store = Store();
        return store.empty() ? "(set " + g + " " + Expr(t, 1) + ")" : store;
    }
    case 7:
        return "(if (= " + count + " " + Literal(count_type) + ") (set " + g + " " + Expr(t, 1) +
               "))";
    case 8:
        return "(if " + BoolExpr(1) + " (set " + g + " " + Expr(t, 1) + ") (set " + CollapseVar(t) +
               " " + As(t, count_type, count) + "))";
    case 9: {
        if (depth <= 0) {
            return "(set " + g + " " + Expr(t, 1) + ")";
        }
        scopes.emplace_back();
        std::string stmt = collapse_rng.Chance(50) ? CollapseLoop(depth - 1) : Stmt(depth - 1);
        scopes.pop_back();
        DropPointers();
        return stmt;
    }
    case 10: {
        if (current == 0) {
            return "(set " + g + " " + Expr(t, 1) + ")";
        }
        std::string call =
            CallText(fns[static_cast<std::size_t>(rng.Below(static_cast<int>(current)))], 1);
        // now and then a global the call may read, set from the counter first
        if (collapse_rng.Chance(40)) {
            return "(do (set " + g + " (^ " + As(t, count_type, count) + " " + Literal(t) + ")) " +
                   call + ")";
        }
        return call;
    }
    case 11:
        // a pass that may end early (a for only: a while's continue could skip its step)
        return is_for && collapse_rng.Chance(50)
                   ? "(if " + BoolExpr(1) + " (break))"
                   : "(if " + BoolExpr(1) + " (set " + g + " " + Literal(t) + "))";
    case 12: {
        // read what the pass writes, before or after
        std::string h = CollapseVar(t);
        return "(do (set " + g + " (+ " + g + " " + h + ")) (set " + h + " " + Expr(t, 1) + "))";
    }
    default: {
        // its own let, read by an accumulator
        std::string v = Fresh("v");
        return "(do (let " + v + " " + Name(t) + " " + Expr(t, 1) + ") (set " + g + " (+ " + g +
               " " + v + ")))";
    }
    }
}

// a for or counting while, its body from CollapseItem
std::string Gen::CollapseLoop(int depth) {
    std::string n = Fresh("n");
    T type = T::U8;
    std::string lets;
    std::string head;
    std::string step;
    bool is_for = true;
    switch (collapse_rng.Below(7)) {
    case 0:
        head = "(for " + n + " u8 0 " + std::to_string(collapse_rng.Below(7)) + " ";
        break;
    case 1:
        head = "(for " + n + " u8 " + std::to_string(collapse_rng.Below(3)) + " (& " +
               NonLit(T::U8, 1) + " 7) ";
        break;
    case 2:
        type = T::I8;
        head = "(for " + n + " i8 -4 (as i8 (& " + NonLit(T::U8, 1) + " 7)) ";
        break;
    case 3:
        type = T::U16;
        head = "(for " + n + " u16 65533 65536 ";
        break;
    case 4:
        head = "(for " + n + " u8 (& " + NonLit(T::U8, 1) + " 7) 5 ";
        break;
    default: {
        is_for = false;
        type = collapse_rng.Chance(70) ? T::U8 : T::I8;
        std::string start = std::to_string(collapse_rng.Below(4));
        std::string k = std::to_string(1 + collapse_rng.Below(6));
        std::string s = std::to_string(1 + collapse_rng.Below(3));
        lets = "(let " + n + " " + Name(type) + " " + start + ") ";
        head = "(while (< " + n + " " + k + ") ";
        step = "(set " + n + " (+ " + n + " " + s + "))";
        break;
    }
    }
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{n, type, true});
    bool was_while = in_while;
    in_while = !is_for;
    loops++;
    std::string body;
    int count = 1 + collapse_rng.Below(4);
    int step_at = collapse_rng.Below(count + 1);
    for (int i = 0; i <= count; i++) {
        if (!is_for && i == step_at) {
            body += " " + step;
        }
        if (i < count) {
            body += " " + CollapseItem(n, type, is_for, depth);
        }
    }
    loops--;
    in_while = was_while;
    scopes.pop_back();
    DropPointers();
    return "(do " + lets + head + body + "))";
}

// a compare a range may decide, near the edge of it, or an and or or with a literal right side
std::string Gen::RangeTest() {
    T t = CollapseInt();
    std::string g = CollapseVar(t);
    std::string x = NonLit(T::U8, 1);
    int mask = (1 << (1 + collapse_rng.Below(6))) - 1;
    std::string shape;
    int top = mask;
    switch (collapse_rng.Below(4)) {
    case 0:
        shape = "(& " + x + " " + std::to_string(mask) + ")";
        break;
    case 1:
        shape = "(% " + x + " " + std::to_string(mask + 1) + ")";
        break;
    case 2:
        shape = "(>> " + x + " " + std::to_string(1 + collapse_rng.Below(7)) + ")";
        top = 127;
        break;
    default:
        shape = "(as i16 (& " + x + " " + std::to_string(mask) + "))";
        break;
    }
    static const char* const kTests[] = {"<", "<=", ">", ">=", "=", "!="};
    std::string k = std::to_string(top - 1 + collapse_rng.Below(3));
    std::string test =
        "(" + std::string(kTests[collapse_rng.Below(6)]) + " " + shape + " " + k + ")";
    if (collapse_rng.Chance(30)) {
        test = "(" + std::string(collapse_rng.Chance(50) ? "and " : "or ") + BoolExpr(1) + " " +
               test + ")";
    }
    return "(if " + test + " (set " + g + " " + Expr(t, 1) + ") (set " + g + " " + Literal(t) +
           "))";
}

// a counting while with a match on its counter: clauses it reaches and ones it cannot
std::string Gen::CounterMatch() {
    std::string c = Fresh("c");
    int k = 1 + collapse_rng.Below(6);
    std::string text = "(do (let " + c + " u8 0) (while (< " + c + " " + std::to_string(k) +
                       ") (set " + c + " (+ " + c + " 1)) (match " + c;
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{c, T::U8, true});
    int used[3] = {-1, -1, -1};
    for (int i = 0; i < 3; i++) {
        int v = collapse_rng.Chance(50) ? collapse_rng.Below(k + 2) : collapse_rng.Below(256);
        bool again = false;
        for (int j = 0; j < i; j++) {
            again = again || used[j] == v;
        }
        if (again) {
            continue;
        }
        used[i] = v;
        T t = CollapseInt();
        text += " (" + std::to_string(v) + " (set " + CollapseVar(t) + " " + Expr(t, 1) + "))";
    }
    text += " (_ (do)))))";
    scopes.pop_back();
    return text;
}

std::string Gen::CollapseShape(int depth) {
    switch (collapse_rng.Below(8)) {
    case 0:
    case 1:
    case 2:
    case 3:
        return CollapseLoop(depth);
    case 4:
    case 5:
        return RangeTest();
    default:
        return CounterMatch();
    }
}

} // namespace tisple_tests::fuzz
