#include "fuzz_gen_internal.hpp"

// I/O through base addresses in globals (backend/io_bases.hpp) and inlined copies that hand
// their parameters on to a call (backend/gen_forward.cpp): u16 globals ib0-ib2 set here and
// there (in helpers too, to numbers and to each other plus a constant), io_read and io_write
// of u8 and u16 at a base plus or minus a constant; ib3 has its address taken, so it is no
// base; small helpers that check and call iput with their own parameters, in an if, after a
// set, with arguments the callers work out (iput changes its parameters). Every access is in
// $C0xx, so the 6502's and the interpreter's I/O logs must match access for access. The
// fuzzer runs these at -O0 too, where the backend makes the copies the mid-level would.

namespace tisple_tests::fuzz {

namespace {

std::string Num(int n) {
    return std::to_string(n);
}

} // namespace

std::string Gen::IoHelpers() {
    Rng& r = io_rng;
    std::string text;
    for (int b = 0; b < 4; b++) {
        text += "(let ib" + Num(b) + " u16 " + Num(0xC010 + r.Below(64)) + ")\n";
    }
    text += "(let iq u8 0)\n";
    // big enough to stay a JSR; it changes its own parameters
    text += "(fn iput ((k u8) (v u8)) void\n    (io_write u8 (+ ib0 " + Num(r.Below(64)) +
            ") k)\n    (io_write u8 ib0 v)\n    (for j u8 0 (& v 3)\n        (set k (+ k j))\n" +
            "        (io_write u8 (- ib1 " + Num(1 + r.Below(15)) + ") (+ k v)))\n" +
            "    (set v (^ v (io_read u8 (+ ib2 " + Num(r.Below(64)) + "))))\n" +
            "    (set iq (+ iq (+ k v))))\n";
    text += "(fn iget ((k u8)) u8 (+ (io_read u8 (+ ib1 " + Num(r.Below(64)) + ")) k))\n";
    // the two parameters handed on: in an if, after a set, swapped
    text += "(fn iwrap ((a u8) (b u8)) void (if (!= a b) (iput a b)))\n";
    text += "(fn iwrap2 ((a u8) (b u8)) void (set iq (+ iq a)) (iput b a))\n";
    text += "(fn iwrap3 ((a u8) (b u8)) void (if (< a " + Num(r.Below(256)) +
            ") (iput a (+ b 1)) (set iq (^ iq b))))\n";
    text += "(fn iwide ((w u16)) void (io_write u16 (+ ib2 " + Num(r.Below(64)) + ") w))\n";
    text +=
        "(fn iset ((x u8)) void (set ib" + Num(r.Below(3)) + " (+ 0xC010 (as u16 (& x 63)))))\n";
    text += "(fn iaddr ((x u8)) void (store (addr_of ib3) (+ 0xC010 (as u16 (& x 63)))))\n";
    return text;
}

// a base's new value: a number, a byte worked out, another base plus a little
std::string Gen::IoBaseValue() {
    Rng& r = io_rng;
    switch (r.Below(3)) {
    case 0:
        return Num(0xC010 + r.Below(64));
    case 1:
        return "(+ 0xC010 (as u16 (& (+ iq " + Expr(T::U8, 1) + ") 63)))";
    default:
        return "(+ 0xC010 (& (+ ib" + Num(r.Below(4)) + " " + Num(r.Below(8)) + ") 63))";
    }
}

// an address at a base: the base, plus or minus a constant (in $C0xx)
std::string Gen::IoBased() {
    Rng& r = io_rng;
    std::string base = "ib" + Num(r.Below(4));
    switch (r.Below(4)) {
    case 0:
        return base;
    case 1:
        return "(- " + base + " " + Num(1 + r.Below(15)) + ")";
    case 2:
        return "(+ " + Num(r.Below(64)) + " " + base + ")";
    default:
        return "(+ " + base + " " + Num(r.Below(64)) + ")";
    }
}

std::string Gen::IoShape(int depth) {
    Rng& r = io_rng;
    switch (r.Below(10)) {
    case 0:
        return "(set ib" + Num(r.Below(4)) + " " + IoBaseValue() + ")";
    case 1:
        return "(io_write u8 " + IoBased() + " " + Expr(T::U8, 1) + ")";
    case 2:
        return "(io_write u16 " + IoBased() + " " + Expr(T::U16, 1) + ")";
    case 3:
        return "(set iq (+ iq (io_read u8 " + IoBased() + ")))";
    case 4:
        return "(set iq (^ iq (as u8 (io_read u16 " + IoBased() + "))))";
    case 5:
        return "(iwrap " + Expr(T::U8, 1) + " " + Expr(T::U8, 1) + ")";
    case 6:
        return "(iwrap" + Num(2 + r.Below(2)) + " " + Expr(T::U8, 1) + " " + Expr(T::U8, 1) + ")";
    case 7:
        return "(set iq (+ iq (iget " + Expr(T::U8, 1) + ")))";
    case 8:
        return r.Chance(50) ? "(iset " + Expr(T::U8, 1) + ")" : "(iaddr " + Expr(T::U8, 1) + ")";
    default:
        if (depth > 0 && r.Chance(50)) {
            return "(if " + BoolExpr(1) + " (do " + IoShape(0) + " " + IoShape(0) + ") " +
                   IoShape(0) + ")";
        }
        return "(iwide " + Expr(T::U16, 1) + ")";
    }
}

} // namespace tisple_tests::fuzz
