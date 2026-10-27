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
        switch (loops_rng.Below(7)) {
        case 0:
            body += " (set " + c + " " + Replace(kByteSteps[loops_rng.Below(6)], c) + ")";
            break;
        case 5:
            body += " (set " + read_below + " (+ " + v + " 7))";
            break;
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
        switch (loops_rng.Below(7)) {
        case 0:
            body += " (set (at wb " + n + ") " + Expr(T::U8, 1) + ")";
            break;
        case 1:
            body += " (store (+ " + q + " " + n + ") (+ (load (+ " + q + " " + n + ")) 1))";
            break;
        case 2:
            body += " (set " + guard + " (+ " + guard + " (at wb " + n + ")))";
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
    // a while that wants Y, so the counter may stay in memory
    std::string body = " (let " + j + " u8 0) (while (< " + j + " " +
                       std::to_string(1 + loops_rng.Below(3)) + ") (set " + j + " (+ " + j +
                       " 1)))";
    int parts = 1 + loops_rng.Below(3);
    for (int n = 0; n < parts && budget > 0; n++) {
        switch (loops_rng.Below(5)) {
        case 0:
            body += " (set (at wb " + i + ") (+ (at wb " + i + ") " + j + "))";
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

std::string Gen::LoopShape(int depth) {
    switch (loops_rng.Below(4)) {
    case 0:
        return ByteWhile(depth);
    case 1:
        return WordWhile(depth);
    case 2:
        return SteppedFor(depth);
    default:
        return WordFor(depth);
    }
}

} // namespace tisple_tests::fuzz
