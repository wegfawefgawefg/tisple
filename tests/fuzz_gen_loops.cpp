#include "fuzz_gen_internal.hpp"

// loops whose variables the backend may keep in registers or step pointers to: while loops
// over u8 and u16 locals (stepped, set in arms, read as indices, after the loop too), u16
// locals stepped in a for body, u16 for loops whose body needs Y. Every loop has a guard
// counter stepped first, so it ends whatever the rest does; indices stay inside their arrays.

namespace tisple_tests::fuzz {

namespace {

const char* const kByteSteps[] = {"(+ {v} 1)", "(- {v} 1)", "(+ {v} 3)",
                                  "(+ 1 {v})", "(- {v} 2)", "(+ {v} 255)"};

std::string Replace(std::string text, const std::string& v) {
    for (std::size_t at = text.find("{v}"); at != std::string::npos; at = text.find("{v}")) {
        text.replace(at, 3, v);
    }
    return text;
}

} // namespace

// a u8 array of 256 to index with any byte, or ""
std::string Gen::ByteTable() {
    for (const ArrayInfo& a : arrays) {
        if (a.elem == T::U8 && a.count == 256 && !a.is_const) {
            return a.name;
        }
    }
    return "";
}

std::string Gen::ByteWhile(int depth) {
    std::string c = Fresh("c");
    std::string v = Fresh("v");
    std::string guard = Fresh("gd");
    std::string table = ByteTable();
    std::string text = "(do (let " + c + " u8 " + NonLit(T::U8, 1) + ") (let " + v + " u8 (& " +
                       NonLit(T::U8, 1) + " 1)) (let " + guard + " u8 0)";
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{c, T::U8, true});
    scopes.back().push_back(VarInfo{v, T::U8, true});
    scopes.back().push_back(VarInfo{guard, T::U8, true});
    std::string limit = std::to_string(2 + loops_rng.Below(30));
    std::string cond = "(< " + guard + " " + limit + ")";
    std::string read = table.empty() ? "(at sm (& " + c + " 31))" : "(at " + table + " " + c + ")";
    std::string read_below =
        table.empty() ? "(at wb (as u16 (- " + c + " 1)))" : "(at " + table + " (- " + c + " 1))";
    switch (loops_rng.Below(4)) {
    case 0:
        // the sort: c moves down while the element below it is not smaller
        cond = "(and " + cond + " (and (!= " + c + " 0) (>= " + read_below + " " + v + ")))";
        break;
    case 1:
        // the search: c moves up while elements match
        cond = "(and " + cond + " (and (< " + c + " 200) (= (at wb (as u16 " + c + ")) " +
               Leaf(T::U8) + ")))";
        break;
    case 2:
        cond = "(and " + cond + " (!= " + c + " " + v + "))";
        break;
    default:
        break;
    }
    std::string body = " (set " + guard + " (+ " + guard + " 1))";
    bool was_while = in_while;
    in_while = false;
    loops++;
    int parts = 1 + loops_rng.Below(4);
    for (int i = 0; i < parts && budget > 0; i++) {
        switch (loops_rng.Below(8)) {
        case 0:
            body += " (set " + c + " " + Replace(kByteSteps[loops_rng.Below(6)], c) + ")";
            break;
        case 5:
            body += " (set " + read_below + " (+ " + v + " 7))";
            break;
        case 6: {
            // a store, a step of the same array at v (INC through the other register), a reread
            std::string at_v =
                table.empty() ? "(at sm (& " + v + " 31))" : "(at " + table + " " + v + ")";
            body += " (set " + read + " (+ " + v + " " + c + ")) (set " + at_v + " (+ " + at_v +
                    " 1)) (set " + v + " " + read + ")";
            break;
        }
        case 1:
            body += " (if " + Expr(T::Bool, 1) + " (set " + c + " " +
                    Replace(kByteSteps[loops_rng.Below(6)], c) + "))";
            break;
        case 2:
            body += " (set " + c + " " + NonLit(T::U8, 2) + ")";
            break;
        case 3:
            body += " (if (!= " + c + " 0) (set " + v + " (+ " + v + " " + read_below + ")))";
            break;
        case 4:
            // the element below c, also where c may be 0 by now: (- c 1) wraps
            body +=
                " (set " + v + " (^ " + v + " " + (loops_rng.Chance(50) ? read : read_below) + "))";
            break;
        default:
            body += Body(depth - 1, 1);
            break;
        }
    }
    body += " (set " + c + " " + Replace(kByteSteps[loops_rng.Below(6)], c) + ")";
    loops--;
    in_while = was_while;
    text += " (while " + cond + body + ")";
    // the values after the loop
    std::vector<const VarInfo*> sinks = VarsOf(T::U8, true);
    if (!sinks.empty()) {
        const std::string& sink =
            sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]->name;
        text += " (set " + sink + " (+ " + c + " " + v + "))";
    }
    scopes.pop_back();
    DropPointers();
    return text + ")";
}

// a while whose test compares a byte it reads against a variable the loop never sets, often
// 255 or 0, the compare last in the test and the byte the body's first value (insertion sort)
std::string Gen::BoundWhile(int depth) {
    std::string c = Fresh("c");
    std::string v = Fresh("v");
    std::string guard = Fresh("gd");
    std::string table = ByteTable();
    std::string bound = loops_rng.Chance(25)   ? std::string("255")
                        : loops_rng.Chance(20) ? std::string("0")
                                               : NonLit(T::U8, 1);
    std::string text = "(do (let " + c + " u8 " + NonLit(T::U8, 1) + ") (let " + v + " u8 " +
                       bound + ") (let " + guard + " u8 0)";
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{c, T::U8, true});
    scopes.back().push_back(VarInfo{guard, T::U8, true});
    std::string read =
        table.empty() ? "(at wb (as u16 " + c + "))" : "(at " + table + " " + c + ")";
    std::string forms[] = {"(> " + read + " " + v + ")", "(<= " + read + " " + v + ")",
                           "(< " + v + " " + read + ")", "(>= " + v + " " + read + ")"};
    std::string cmp = forms[loops_rng.Below(4)];
    std::string limit = std::to_string(2 + loops_rng.Below(40));
    std::string cond = loops_rng.Chance(20) ? "(and " + cmp + " (< " + guard + " " + limit + "))"
                                            : "(and (< " + guard + " " + limit + ") " + cmp + ")";
    std::vector<const VarInfo*> sinks = VarsOf(T::U8, true);
    std::string body;
    if (!sinks.empty()) {
        const std::string& sink =
            sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]->name;
        body += " (set " + sink + " " + read + ")";
    }
    body += " (set " + guard + " (+ " + guard + " 1))";
    bool was_while = in_while;
    in_while = false;
    loops++;
    if (loops_rng.Chance(30)) {
        body += " (if " + Expr(T::Bool, 1) + (loops_rng.Chance(50) ? " (break))" : " (continue))");
    }
    if (loops_rng.Chance(40) && budget > 0) {
        body += Body(depth - 1, 1);
    }
    body += " (set " + c + " " + Replace(kByteSteps[loops_rng.Below(6)], c) + ")";
    loops--;
    in_while = was_while;
    text += " (while " + cond + body + ")";
    if (!sinks.empty()) {
        const std::string& sink =
            sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]->name;
        text += " (set " + sink + " (+ " + c + " " + guard + "))";
    }
    scopes.pop_back();
    DropPointers();
    return text + ")";
}

// a Bresenham step: a local only ever 1 or -1 moving a variable, and a 16-bit let compared
// right after it is worked out (its high byte still in A)
std::string Gen::StepWhile(int depth) {
    // now and then a u16 index into wb (page pointers that follow it), kept inside wb: it
    // starts at 300 to 555 and moves by at most 250. It reads and writes wb there (a write
    // through a stale pointer shows in memory, where wb's zeros would hide a read), and reads
    // through a pointer with Y, which keeps the u16 out of Y
    bool table = loops_rng.Chance(40);
    T t = table ? T::U16 : kInts[loops_rng.Below(4)];
    T wide_t = loops_rng.Chance(50) ? T::I16 : T::U16;
    bool is_signed = t == T::I8 || t == T::I16;
    std::string minus_one = is_signed ? "-1" : (t == T::U8 ? "255" : "65535");
    std::string s = Fresh("s");
    std::string v = Fresh("v");
    std::string tw = Fresh("tw");
    std::string guard = Fresh("gd");
    std::string q = Fresh("q");
    std::string text =
        "(do (let " + s + " " + Name(t) + " " +
        (loops_rng.Chance(50) ? std::string("1") : minus_one) + ") (if " + Expr(T::Bool, 1) +
        " (set " + s + " " + (loops_rng.Chance(50) ? minus_one : std::string("1")) + ")) (let " +
        v + " " + Name(t) + " " + (table ? "(+ (as u16 " + NonLit(T::U8, 1) + ") 300)" : Leaf(t)) +
        ") (let " + guard + " u8 0) (let " + q + " (ptr u8) (addr_of (at wb 3)))";
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{v, t, true});
    std::string limit = std::to_string(1 + loops_rng.Below(250));
    std::string step = loops_rng.Chance(50) ? "(+ " + v + " " + s + ")" : "(+ " + s + " " + v + ")";
    std::string widened = t == wide_t ? v : "(as " + std::string(Name(wide_t)) + " " + v + ")";
    std::string twice = loops_rng.Chance(60) ? "(<< " + widened + " 1)"
                                             : "(- " + widened + " " + Leaf(wide_t) + ")";
    const char* forms[] = {">=", "<", ">", "<="};
    const char* op = forms[loops_rng.Below(4)];
    std::string other = Leaf(wide_t);
    std::string cmp = loops_rng.Chance(70) ? "(" + std::string(op) + " " + tw + " " + other + ")"
                                           : "(" + std::string(op) + " " + other + " " + tw + ")";
    std::vector<const VarInfo*> sinks = VarsOf(T::U8, true);
    std::string body = " (set " + v + " " + step + ") (let " + tw + " " + Name(wide_t) + " " +
                       twice + ") (set " + guard + " (+ " + guard + " 1))";
    if (!sinks.empty()) {
        const std::string& sink =
            sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]->name;
        body = " (set " + v + " " + step + ") (let " + tw + " " + Name(wide_t) + " " + twice +
               ") (if " + cmp + " (set " + sink + " (+ " + sink + " 1))) (set " + guard + " (+ " +
               guard + " 1))";
    }
    if (table && !sinks.empty()) {
        const std::string& sink =
            sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]->name;
        std::string read = " (set " + sink + " (+ " + sink + " (at wb " + v + ")))";
        std::string write = " (set (at wb " + v + ") (+ (at wb " + v + ") " + guard + "))";
        std::string by_y =
            " (set " + sink + " (^ " + sink + " (load (+ " + q + " " + guard + "))))";
        body = (loops_rng.Chance(50) ? read : std::string()) + body + read + write +
               (loops_rng.Chance(70) ? by_y : std::string());
    }
    bool was_while = in_while;
    in_while = false;
    loops++;
    if (loops_rng.Chance(30) && budget > 0) {
        body += Body(depth - 1, 1);
    }
    loops--;
    in_while = was_while;
    text += " (while (< " + guard + " " + limit + ")" + body + ")";
    if (!sinks.empty()) {
        const std::string& sink =
            sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]->name;
        text += " (set " + sink + " (^ " + sink + " (as u8 " + v + ")))";
    }
    scopes.pop_back();
    DropPointers();
    return text + ")";
}

// a scan (strlen): a u16 stepped by one and nothing else, from odd and even starts, often
// across pages of wb
std::string Gen::ScanWhile() {
    std::string n = Fresh("n");
    std::string start = "(as u16 " + NonLit(T::U8, 1) + ")";
    if (loops_rng.Chance(30)) {
        start = "(+ " + start + " " + std::to_string(loops_rng.Below(400)) + ")";
    }
    std::string limit = std::to_string(1 + loops_rng.Below(1000));
    std::string stop = loops_rng.Chance(50) ? std::string("90") : Leaf(T::U8);
    std::string read = loops_rng.Chance(70)
                           ? "(at wb " + n + ")"
                           : "(at wb (+ " + n + " " + std::to_string(loops_rng.Below(20)) + "))";
    std::string text = "(do (let " + n + " u16 " + start + ") (while (and (< " + n + " " + limit +
                       ") (!= " + read + " " + stop + ")) (set " + n + " (+ " + n + " 1)))";
    std::vector<const VarInfo*> sinks = VarsOf(T::U16, true);
    if (!sinks.empty()) {
        const std::string& sink =
            sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]->name;
        text += " (set " + sink + " (+ " + sink + " " + n + "))";
    }
    return text + ")";
}

std::string Gen::WordWhile(int depth) {
    std::string n = Fresh("n");
    std::string d = Fresh("d");
    std::string q = Fresh("q");
    std::string guard = Fresh("gd");
    // n stays below 760 where the loop reads wb at n, so a pointer up to 255 into wb reads
    // inside it too
    bool down = loops_rng.Chance(30);
    std::string start =
        down ? "(+ 300 (as u16 " + NonLit(T::U8, 1) + "))" : "(as u16 " + NonLit(T::U8, 1) + ")";
    std::string text = "(do (let " + n + " u16 " + start + ") (let " + d + " u16 (as u16 (+ (& " +
                       NonLit(T::U8, 1) + " 15) 1))) (let " + q +
                       " (ptr u8) (addr_of (at wb (as u16 " + NonLit(T::U8, 1) + ")))) (let " +
                       guard + " u8 0)";
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{n, T::U16, true});
    scopes.back().push_back(VarInfo{d, T::U16, true});
    scopes.back().push_back(VarInfo{guard, T::U8, true});
    // at most two steps a pass: up from below 300 by 200 at most, down from above 260 by 100
    // at most, so n stays below 700 and a pointer up to 255 into wb reads inside it
    static const char* const kUp[] = {"(+ {v} 1)", "(+ {v} 2)", "(+ {v} 128)", "(+ {v} 200)",
                                      "(+ 1 {v})"};
    static const char* const kDown[] = {"(- {v} 1)", "(- {v} 3)", "(- {v} 100)"};
    std::string step =
        down ? Replace(kDown[loops_rng.Below(3)], n) : Replace(kUp[loops_rng.Below(5)], n);
    int kind = loops_rng.Below(4);
    if (!down && kind == 1) {
        step = "(+ " + n + " " + d + ")";
    } else if (!down && kind == 2) {
        step = "(& (+ (* " + n + " 3) " + d + ") 511)";
    }
    std::string limit = down ? "(> " + n + " 260)" : "(< " + n + " 300)";
    // now and then n is only an index stepped by one (strlen): the guard alone bounds it
    bool index_only = loops_rng.Chance(25);
    if (index_only) {
        step = down ? "(- " + n + " 1)" : "(+ " + n + " 1)";
        limit = "(!= (at wb " + n + ") " + Leaf(T::U8) + ")";
    }
    std::string cond =
        "(and (< " + guard + " " + std::to_string(2 + loops_rng.Below(40)) + ") " + limit + ")";
    if (loops_rng.Chance(40)) {
        cond = "(and " + cond + " (!= (at wb " + n + ") " + Leaf(T::U8) + "))";
    }
    std::string body = " (set " + guard + " (+ " + guard + " 1))";
    bool was_while = in_while;
    in_while = false;
    loops++;
    int parts = 1 + loops_rng.Below(4);
    bool stepped = false;
    for (int i = 0; i < parts && budget > 0; i++) {
        switch (loops_rng.Below(index_only ? 2 : 7)) {
        case 0:
            body += " (set (at wb " + n + ") " + Expr(T::U8, 1) + ")";
            break;
        case 1:
            body += " (store (+ " + q + " " + n + ") (+ (load (+ " + q + " " + n + ")) 1))";
            break;
        case 2:
            // n stays below 700 here: n + 300 inside wb
            body += " (set " + guard + " (+ " + guard + " (at wb " + n + "))) (set (at wb (+ " + n +
                    " 300)) (at wb (- (+ " + n + " 5) 5)))";
            break;
        case 3:
            if (!stepped) {
                body += " (if " + Expr(T::Bool, 1) + " (set " + n + " " + step + "))";
                stepped = true;
            }
            break;
        case 4:
            body += " (if " + Expr(T::Bool, 1) + " (continue))";
            break;
        case 5: {
            std::vector<const VarInfo*> sinks = VarsOf(T::U8, true);
            if (!sinks.empty()) {
                const std::string& sink =
                    sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]
                        ->name;
                body += " (set " + sink + " (+ " + sink + " (load (+ " + q + " " + n + "))))";
            }
            break;
        }
        default:
            body += Body(depth - 1, 1);
            break;
        }
    }
    body += " (set " + n + " " + step + ")";
    loops--;
    in_while = was_while;
    text += " (while " + cond + body + ")";
    std::vector<const VarInfo*> sinks = VarsOf(T::U16, true);
    if (!sinks.empty()) {
        const std::string& sink =
            sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]->name;
        text += " (set " + sink + " (^ " + sink + " " + n + "))";
    }
    scopes.pop_back();
    DropPointers();
    return text + ")";
}

std::string Gen::SteppedFor(int depth) {
    std::string k = Fresh("k");
    std::string x = Fresh("x");
    bool down = loops_rng.Chance(30);
    // up to 16 steps of at most 40 from below 256, or down from 512 or more: inside wb
    std::string start =
        down ? "(+ 512 (as u16 " + NonLit(T::U8, 1) + "))" : "(as u16 " + NonLit(T::U8, 1) + ")";
    static const char* const kUp[] = {"1", "2", "3", "40", "17"};
    static const char* const kDown[] = {"1", "5", "30"};
    std::string amount = down ? kDown[loops_rng.Below(3)] : kUp[loops_rng.Below(5)];
    std::string step = std::string("(") + (down ? "-" : "+") + " " + k + " " + amount + ")";
    std::string text = "(do (let " + k + " u16 " + start + ")";
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{k, T::U16, true});
    std::string head = "(for " + x + " u8 0 " + std::to_string(1 + loops_rng.Below(16));
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{x, T::U8, true});
    std::string outer_counter = counter;
    counter = x;
    loops++;
    std::string body;
    int parts = 1 + loops_rng.Below(4);
    bool stepped = false;
    for (int i = 0; i < parts && budget > 0; i++) {
        switch (loops_rng.Below(6)) {
        case 0:
            body += " (set (at wb " + k + ") " + Expr(T::U8, 1) + ")";
            // k stays at 32 or more going down and below 896 going up: these are inside wb
            body += down ? " (set (at wb (- " + k + " 30)) (at wb (- " + k + " 1)))"
                         : " (set (at wb (+ " + k + " 100)) (at wb (+ 7 " + k + ")))";
            break;
        case 1:
            body += stepped ? "" : " (set " + k + " " + step + ")";
            stepped = true;
            break;
        case 2:
            body += stepped ? "" : " (if " + Expr(T::Bool, 1) + " (set " + k + " " + step + "))";
            stepped = true;
            break;
        case 3:
            body +=
                " (for " + Fresh("m") + " u8 0 2 (set (at wb " + k + ") (+ (at wb " + k + ") 1)))";
            break;
        case 4: {
            std::vector<const VarInfo*> sinks = VarsOf(T::U8, true);
            if (!sinks.empty()) {
                const std::string& sink =
                    sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]
                        ->name;
                body += " (set " + sink + " (^ " + sink + " (at wb " + k + ")))";
            }
            break;
        }
        default:
            body += Body(depth - 1, 1);
            break;
        }
    }
    // at most one step a pass, wherever it is
    if (!stepped) {
        body += " (set " + k + " " + step + ")";
    }
    loops--;
    counter = outer_counter;
    scopes.pop_back();
    text += " " + head + body + ")";
    std::vector<const VarInfo*> sinks = VarsOf(T::U16, true);
    if (!sinks.empty()) {
        const std::string& sink =
            sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]->name;
        text += " (set " + sink + " (+ " + sink + " " + k + "))";
    }
    scopes.pop_back();
    DropPointers();
    return text + ")";
}

std::string Gen::WordFor(int depth) {
    std::string i = Fresh("w");
    std::string j = Fresh("j");
    std::string start = std::to_string(loops_rng.Below(300));
    std::string end = std::to_string(300 + loops_rng.Below(212));
    std::string head = "(for " + i + " u16 " + start + " " + end;
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{i, T::U16, true});
    loops++;
    std::vector<const VarInfo*> words = VarsOf(T::U16, true);
    if (loops_rng.Chance(30) && !words.empty()) {
        // the search: the counter read only where it matches, a pointer that moves with it
        const std::string& sink =
            words[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(words.size())))]->name;
        std::string p = Fresh("p");
        // wb is mostly zeros, so a 0 matches often
        std::string byte = loops_rng.Chance(50) ? std::string("0") : Leaf(T::U8);
        std::string body = " (let " + p + " (ptr u8) (addr_of (at wb " + i + "))) (let " + j +
                           " u8 0) (while (and (< " + j + " 3) (= (load (+ " + p + " " + j + ")) " +
                           byte + ")) (set " + j + " (+ " + j + " 1)))";
        if (loops_rng.Chance(30)) {
            body += " (if " + Expr(T::Bool, 1) + " (continue))";
        }
        // a plain copy of the counter is cheap enough that the counter goes
        std::string found = loops_rng.Chance(50) ? "(set " + sink + " " + i + ")"
                                                 : "(set " + sink + " (+ " + sink + " " + i + "))";
        if (loops_rng.Chance(30)) {
            found = "(do " + found + " (break))";
        }
        body += " (if (= " + j + " 3) " + found + ")";
        loops--;
        scopes.pop_back();
        DropPointers();
        return head + body + ")";
    }
    // a while that wants Y, so the counter may stay in memory
    std::string body = " (let " + j + " u8 0) (while (< " + j + " " +
                       std::to_string(1 + loops_rng.Below(3)) + ") (set " + j + " (+ " + j +
                       " 1)))";
    int parts = 1 + loops_rng.Below(3);
    for (int n = 0; n < parts && budget > 0; n++) {
        switch (loops_rng.Below(6)) {
        case 0:
            body += " (set (at wb " + i + ") (+ (at wb " + i + ") " + j + "))";
            break;
        case 4:
            // constants around the counter (it is below 511), which move into the address
            body += " (set (at wb (+ " + i + " 500)) (+ (at wb (- (+ " + i + " 3) 1)) (at wb (+ " +
                    std::to_string(loops_rng.Below(12)) + " " + i + ")))) (set (at ww (- (+ " + i +
                    " 1) 1)) (+ (at ww (+ 0 " + i + ")) 1))";
            break;
        case 1: {
            std::string p = Fresh("p");
            body += " (let " + p + " (ptr u8) (addr_of (at wb " + i + "))) (store " + p +
                    " (+ (load " + p + ") 1))";
            break;
        }
        case 2: {
            std::string p = Fresh("p");
            body += " (let " + p + " (ptr u16) (addr_of (at ww " + i + "))) (store " + p +
                    " (+ (load " + p + ") " + i + "))";
            break;
        }
        case 3:
            body += " (if " + Expr(T::Bool, 1) + " (continue))";
            break;
        default:
            body += Body(depth - 1, 1);
            break;
        }
    }
    loops--;
    scopes.pop_back();
    DropPointers();
    return head + body + ")";
}

std::string Gen::BitSteps(int depth) {
    bool wide16 = loops_rng.Chance(60);
    const char* type = wide16 ? "u16" : "u8";
    std::string top = wide16 ? "0x8000" : "0x80";
    std::string v = Fresh("cr");
    std::string text =
        std::string("(do (let ") + v + " " + type + " " + NonLit(wide16 ? T::U16 : T::U8, 1) + ")";
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{v, wide16 ? T::U16 : T::U8, true});
    std::string k = std::to_string(loops_rng.Below(wide16 ? 65536 : 256));
    std::string shift = "(<< " + v + " 1)";
    // the CRC step: the bit set or clear, either way round, sometimes more in an arm
    std::string with = "(^ " + shift + " " + k + ")";
    if (loops_rng.Chance(30)) {
        with = "(+ " + with + " " + Leaf(wide16 ? T::U16 : T::U8) + ")";
    }
    std::string test = loops_rng.Chance(50) ? "(!= (& " + v + " " + top + ") 0)"
                                            : "(= 0 (& " + top + " " + v + "))";
    bool set_first = test[1] == '!';
    std::string arms = set_first ? " (set " + v + " " + with + ") (set " + v + " " + shift + ")"
                                 : " (set " + v + " " + shift + ") (set " + v + " " + with + ")";
    std::string bit = Fresh("b");
    text += " (for " + bit + " u8 0 " + std::to_string(1 + loops_rng.Below(9)) + " (if " + test +
            arms + ")";
    // masks of other bits tested, both ways
    if (loops_rng.Chance(60)) {
        std::string mask = std::to_string(loops_rng.Below(wide16 ? 65536 : 256));
        std::vector<const VarInfo*> sinks = VarsOf(T::U8, true);
        if (!sinks.empty()) {
            const std::string& sink =
                sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]
                    ->name;
            text += " (if (" + std::string(loops_rng.Chance(50) ? "=" : "!=") + " (& " + v + " " +
                    mask + ") 0) (set " + sink + " (+ " + sink + " 1)))";
        }
    }
    text += Body(depth - 1, 1) + ")";
    std::vector<const VarInfo*> sinks = VarsOf(wide16 ? T::U16 : T::U8, true);
    if (!sinks.empty()) {
        const std::string& sink =
            sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]->name;
        text += " (set " + sink + " (^ " + sink + " " + v + "))";
    }
    scopes.pop_back();
    DropPointers();
    return text + ")";
}

std::string Gen::LoopShape(int depth) {
    switch (loops_rng.Below(8)) {
    case 0:
        return ByteWhile(depth);
    case 7:
        return ScanWhile();
    case 5:
        return BoundWhile(depth);
    case 6:
        return StepWhile(depth);
    case 1:
        return WordWhile(depth);
    case 2:
        return SteppedFor(depth);
    case 3:
        return BitSteps(depth);
    default:
        return WordFor(depth);
    }
}

} // namespace tisple_tests::fuzz
