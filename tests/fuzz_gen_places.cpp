#include "fuzz_gen_internal.hpp"

// Places reached through known addresses (opt/pointers.cpp, backend gen_place.cpp and
// gen_struct.cpp): a 10-byte struct PR, globals pa and pb of it and an array parr; global
// pointers that nothing sets (pg0 at pa, pg1 at an element of parr, pgw into the u16 array
// pw) and one that shapes set (pg2); local pointers one let sets to (addr_of pa) or pb.
// Struct copies through all of them (the same place too, which a loop copies in place), loads
// and stores of fields, and helpers that take the pointers.

namespace tisple_tests::fuzz {

namespace {

std::string Num(int n) {
    return std::to_string(n);
}

} // namespace

std::string Gen::PlacesHelpers() {
    Rng& r = places_rng;
    std::string text = "(struct PR ((nx (ptr PR)) (k u8) (n i16) (tag (array u8 5))))\n"
                       "(let pa PR)\n(let pb PR)\n(let parr (array PR 4))\n(let pw (array u16 6))\n"
                       "(let pacc u16 0)\n";
    text += "(let pg0 (ptr PR) (addr_of pa))\n";
    text += "(let pg1 (ptr PR) (addr_of (at parr " + Num(r.Below(4)) + ")))\n";
    text += "(let pg2 (ptr PR) (addr_of pb))\n";
    text += "(let pgw (ptr u16) (addr_of (at pw " + Num(r.Below(5)) + ")))\n";
    text += "(fn pcopy ((d (ptr PR)) (s (ptr PR))) void (store d (load s)))\n";
    text += "(fn pbump ((p (ptr PR)) (v u8)) void\n    (store p k (+ (load p k) v))\n"
            "    (store p n (+ (load p n) (as i16 v))))\n";
    return text;
}

// a pointer to a PR
std::string Gen::PlacesPointer() {
    Rng& r = places_rng;
    switch (r.Below(6)) {
    case 0:
        return "pg0";
    case 1:
        return "pg1";
    case 2:
        return "pg2";
    case 3:
        return "(addr_of pa)";
    case 4:
        return "(addr_of pb)";
    default:
        return "(addr_of (at parr (& " + Expr(T::U8, 1) + " 3)))";
    }
}

std::string Gen::PlacesShape(int depth) {
    Rng& r = places_rng;
    switch (r.Below(11)) {
    case 0:
        return "(store " + PlacesPointer() + " (load " + PlacesPointer() + "))";
    case 1:
        return "(pcopy " + PlacesPointer() + " " + PlacesPointer() + ")";
    case 2:
        return "(pbump " + PlacesPointer() + " " + Expr(T::U8, 1) + ")";
    case 3:
        return r.Chance(50) ? "(store " + PlacesPointer() + " k " + Expr(T::U8, 1) + ")"
                            : "(store " + PlacesPointer() + " n " + Expr(T::I16, 1) + ")";
    case 4:
        return "(set pacc (+ pacc (+ (as u16 (load " + PlacesPointer() + " n)) (as u16 (load " +
               PlacesPointer() + " k)))))";
    case 5:
        switch (r.Below(3)) {
        case 0:
            return "(store pgw (+ (load pgw) " + Expr(T::U16, 1) + "))";
        case 1:
            return "(store (+ pgw 1) " + Expr(T::U16, 1) + ")";
        default:
            return "(set pacc (^ pacc (load (+ pgw 1))))";
        }
    case 6:
        return "(set pg2 " + PlacesPointer() + ")";
    case 7: {
        std::string lp = Fresh("lp");
        std::string target = r.Chance(50) ? "pa" : "pb";
        return "(do (let " + lp + " (ptr PR) (addr_of " + target + ")) (store " + lp +
               " k (+ (load " + lp + " k) " + Expr(T::U8, 1) +
               ")) (set pacc (+ pacc (as u16 (load " + lp + " n)))) (store " + lp + " (load " +
               PlacesPointer() + ")))";
    }
    case 8:
        return r.Chance(50) ? "(set (at parr (& " + Expr(T::U8, 1) + " 3)) pa)"
                            : "(set pb (at parr (& " + Expr(T::U8, 1) + " 3)))";
    case 9:
        return "(store " + PlacesPointer() + " nx " + PlacesPointer() + ")";
    default:
        if (depth > 0) {
            return "(if " + BoolExpr(1) + " " + PlacesShape(0) + " " + PlacesShape(0) + ")";
        }
        return "(set pacc (+ pacc (as u16 pa.k)))";
    }
}

} // namespace tisple_tests::fuzz
