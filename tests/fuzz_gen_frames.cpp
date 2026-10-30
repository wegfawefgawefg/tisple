#include "fuzz_gen_internal.hpp"

// Where the bytes of inlined copies' locals go (backend/frame_share.hpp): small helpers with
// locals of their own (bytes, words, a pointer read through), copied at every call: copies of
// one helper inside each other's arguments, copies of different helpers whose values are
// needed at once, a copy's value held across the call of a small helper with a loop called in
// several places (a JSR, whose frame overlaps the copies' when they share a frame), copies in
// loops around such calls, one helper called in one place that holds copies of the others,
// and the same helpers copied into every function.

namespace tisple_tests::fuzz {

namespace {

std::string Num(int n) {
    return std::to_string(n);
}

} // namespace

std::string Gen::FramesHelpers() {
    Rng& r = frames_rng;
    std::string text = "(let rg u8 " + Num(r.Below(256)) + ")\n(let rh u16 " +
                       Num(static_cast<int>(r.Next() % 65536)) +
                       ")\n(let rt (array u8 16) (make "
                       "(array u8 16)";
    for (int i = 0; i < 16; i++) {
        text += " " + Num(r.Below(256));
    }
    text += "))\n";
    text += "(fn ra ((x u8) (y u8)) u8 (let t u8 (+ x rg)) (let u u8 (^ t y)) (+ (* t " +
            Num(1 + r.Below(7)) + ") u))\n";
    text += "(fn rb ((p u16) (k u8)) u16 (let s u16 (+ p (as u16 k))) (set rg (+ rg " +
            Num(1 + r.Below(5)) + ")) (^ s (>> p " + Num(1 + r.Below(5)) + ")))\n";
    // a pointer local read through: zero page
    text += "(fn rc ((k u8)) u8 (let p (ptr u8) (addr_of (at rt (& k 15)))) (let v u8 (load p))"
            " (+ v (at rt (& (+ k " +
            Num(1 + r.Below(15)) + ") 15))))\n";
    text +=
        "(fn rd ((x u8)) u16 (let w u16 (* (as u16 x) " + Num(3 + r.Below(60)) + ")) (+ w rh))\n";
    // small, with a loop that stores (so no pass works it out), called in several places: a JSR
    text += "(fn rl ((n u8)) u8 (let acc u8 " + Num(r.Below(9)) +
            ")\n    (for i u8 0 (& n 7) (set acc (+ acc (at rt i))) (set (at rt (& (+ i n) 15)) "
            "acc))\n    (set rh (+ rh (as u16 acc))) acc)\n";
    // its value a local of its own: a copy's value read where it is, after what comes next
    text += "(fn rv ((x u8)) u8 (let t u8 (+ x rg)) (set rh (+ rh (as u16 t))) t)\n";
    // the same as rl with no parameters: nothing written just before its call
    text += "(fn rz () u8 (let acc u8 rg)\n    (for i u8 0 (& (as u8 rh) 7) (set acc (+ acc (at "
            "rt i))) (set (at rt (& (+ i rg) 15)) acc))\n    (set rg (+ acc " +
            Num(1 + r.Below(9)) + ")) acc)\n";
    // called in one place at most: copied there, with copies of the others in it
    text += "(fn ro ((x u8) (y u8)) u8\n    (let a u8 (ra x y))\n    (let b u16 (rb (rd a) y))\n"
            "    (let c u8 (rl a))\n    (set rh (^ rh b))\n    (+ a (+ c (rc (as u8 b)))))\n";
    return text;
}

std::string Gen::FramesShape(int depth) {
    Rng& r = frames_rng;
    std::string x = Expr(T::U8, 1);
    std::string y = r.Chance(50) ? Var(T::U8) : Expr(T::U8, 1);
    switch (r.Below(16)) {
    case 13:
        return "(set rg (+ (rv " + x + ") (rz)))";
    case 14:
        return "(set rg (^ (rv " + x + ") (rl " + y + ")))";
    case 15:
        return "(set rh (+ (as u16 (rv " + x + ")) (rb rh (rv " + y + "))))";
    case 10:
        return "(set rg (+ (ra " + x + " " + y + ") (rz)))";
    case 11:
        return "(set rh (+ (rd (ra " + x + " rg)) (as u16 (rz))))";
    case 12: {
        std::string i = Fresh("ri");
        return "(for " + i + " u8 0 " + Num(2 + r.Below(12)) + " (set rg (+ (rc (ra " + i + " " +
               y + ")) (rz))))";
    }
    case 0:
        return "(set rg (ra (ra " + x + " " + y + ") (ra " + y + " " + x + ")))";
    case 1:
        return "(set rg (+ (ra " + x + " " + y + ") (rl " + y + ")))";
    case 2:
        return "(set rh (rb (rd " + x + ") (ra " + y + " " + x + ")))";
    case 3:
        return "(set rg (+ (rc " + x + ") (rc (ra " + x + " " + y + "))))";
    case 4: {
        std::string i = Fresh("ri");
        return "(for " + i + " u8 0 " + Num(2 + r.Below(12)) + " (set rh (+ rh (rd (ra " + i +
               " rg)))) (set rg (^ rg (rl " + i + "))))";
    }
    case 5: {
        std::string v = Fresh("rv");
        return "(do (let " + v + " u8 (ra " + x + " " + y + ")) (set rg (+ (rl " + v + ") " + v +
               ")))";
    }
    case 6:
        if (!frames_once) {
            frames_once = true;
            return "(set rg (+ rg (ro " + x + " " + y + ")))";
        }
        return "(set rh (+ (rb rh (rl " + x + ")) (rd (rc " + y + "))))";
    case 7:
        return "(set rh (+ (rb rh (rl " + x + ")) (rd (rc " + y + "))))";
    case 8:
        if (depth > 0) {
            return "(if " + BoolExpr(1) + " (set rg (rc (ra rg " + x + "))) " +
                   Arm(depth - 1, 1, false) + ")";
        }
        return "(set rg (rc (ra rg " + x + ")))";
    default:
        return "(set rh (^ (rd (ra " + x + " (rl " + y + "))) (rb (rd " + y + ") rg)))";
    }
}

} // namespace tisple_tests::fuzz
