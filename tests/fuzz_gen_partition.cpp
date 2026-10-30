#include "fuzz_gen_internal.hpp"

// Hoare's partition, the shape of the sort race's quicksort: an outer loop (a while with a
// guard, or a u8 for) that steps two u16 indexes, i up and j down, with inner whiles moving
// them first, then loads and stores through the outer indexes. The outer loop's step pointers
// for i and j must follow the inner loops, which keep the index in Y. Bounds: i starts below
// 200 and its inner loop stops at hi (at most 440), j starts at most 511 and its inner loop
// stops at lo (at least 60); each moves at most 3 more a step and 1 an outer pass, for at most
// 40 passes, so both stay inside ww (512) and wb (1024), and j - 1 inside too.

namespace tisple_tests::fuzz {

std::string Gen::PartitionLoop(int depth) {
    std::string i = Fresh("pi");
    std::string j = Fresh("pj");
    std::string guard = Fresh("gd");
    bool words = loops_rng.Chance(60);
    std::string arr = words ? "ww" : "wb";
    T elem = words ? T::U16 : T::U8;
    std::string hi = std::to_string(256 + loops_rng.Below(185));
    std::string lo = std::to_string(60 + loops_rng.Below(200));
    std::string text = "(do (let " + i + " u16 (as u16 (% " + NonLit(T::U8, 1) + " 200))) (let " +
                       j + " u16 (- 511 (as u16 (& " + NonLit(T::U8, 1) + " 127))))";
    std::string pivot = Leaf(elem);
    if (loops_rng.Chance(50)) {
        std::string p = Fresh("pv");
        text += " (let " + p + " " + Name(elem) + " " + NonLit(elem, 1) + ")";
        pivot = p;
    }
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{i, T::U16, true});
    scopes.back().push_back(VarInfo{j, T::U16, true});
    static const char* const kSteps[] = {"1", "1", "2", "3"};
    std::string up = kSteps[loops_rng.Below(4)];
    std::string down = kSteps[loops_rng.Below(4)];
    std::string scan_i = " (while (and (< " + i + " " + hi + ") (< (at " + arr + " " + i + ") " +
                         pivot + ")) (set " + i + " (+ " + i + " " + up + ")))";
    std::string scan_j = " (while (and (> " + j + " " + lo + ") (> (at " + arr + " " + j + ") " +
                         pivot + ")) (set " + j + " (- " + j + " " + down + ")))";
    std::string body;
    if (loops_rng.Chance(85)) {
        body += scan_i;
    }
    if (loops_rng.Chance(85)) {
        body += scan_j;
    }
    if (loops_rng.Chance(40)) {
        body += " (if (>= " + i + " " + j + ") (break))";
    }
    bool was_while = in_while;
    in_while = false;
    loops++;
    // the swap, or stores through one index or both, and reads of them
    switch (loops_rng.Below(4)) {
    case 0: {
        std::string t = Fresh("t");
        body += " (let " + t + " " + Name(elem) + " (at " + arr + " " + i + ")) (set (at " + arr +
                " " + i + ") (at " + arr + " " + j + ")) (set (at " + arr + " " + j + ") " + t +
                ")";
        break;
    }
    case 1:
        body += " (set (at " + arr + " " + j + ") " + Expr(elem, 1) + ")";
        break;
    case 2:
        body += " (set (at " + arr + " " + i + ") (+ (at " + arr + " " + i + ") 1)) (set (at " +
                arr + " (- " + j + " 1)) (at " + arr + " " + j + "))";
        break;
    default:
        body += " (set (at " + arr + " " + j + ") (at " + arr + " " + i + "))";
        break;
    }
    if (loops_rng.Chance(25) && budget > 0) {
        body += Body(depth - 1, 1);
    }
    loops--;
    in_while = was_while;
    if (loops_rng.Chance(80)) {
        body += " (set " + i + " (+ " + i + " 1))";
    }
    if (loops_rng.Chance(80)) {
        body += " (set " + j + " (- " + j + " 1))";
    }
    std::string passes = std::to_string(1 + loops_rng.Below(40));
    if (loops_rng.Chance(30)) {
        text += " (for " + guard + " u8 0 " + passes + body + ")";
    } else {
        text += " (let " + guard + " u8 0) (while (< " + guard + " " + passes + ") (set " + guard +
                " (+ " + guard + " 1))" + body + ")";
    }
    std::vector<const VarInfo*> sinks = VarsOf(T::U16, true);
    if (!sinks.empty()) {
        const std::string& sink =
            sinks[static_cast<std::size_t>(loops_rng.Below(static_cast<int>(sinks.size())))]->name;
        text += " (set " + sink + " (+ " + sink + " (^ " + i + " (<< " + j + " 3))))";
    }
    scopes.pop_back();
    DropPointers();
    return text + ")";
}

} // namespace tisple_tests::fuzz
