#include "fuzz_gen_internal.hpp"

// Functions in separate stretches of memory (backend/link_code.hpp): :reserve cuts every few
// hundred bytes from just past the startup, so the functions the program holds land in
// different stretches, and helpers that end in a call that stays a call, (if c (sbig0)) last
// and (if c (do (sbig1 c) (return))), which the peephole makes a branch into the callee
// (the branch over a JMP): it must be the long form when the callee is in another stretch.
// The tail helpers are sometimes small enough to be copied into their callers.

namespace tisple_tests::fuzz {

namespace {

std::string Num(int n) {
    return std::to_string(n);
}

} // namespace

// a statement of a big helper's body on sbuf and sg (k: its parameter, or "")
std::string Gen::StretchWork(const std::string& k) {
    Rng& r = stretch_rng;
    std::string at = "(at sbuf " + Num(r.Below(64)) + ")";
    std::string other = "(at sbuf " + Num(r.Below(64)) + ")";
    std::string with = k.empty() || r.Chance(50) ? "sg" : k;
    switch (r.Below(4)) {
    case 0:
        return "(set " + at + " (+ " + other + " " + with + "))";
    case 1:
        return "(set sg (^ sg " + at + "))";
    case 2:
        return "(set " + at + " (- " + at + " " + Num(1 + r.Below(200)) + "))";
    default:
        return "(set sw (+ sw (as u16 " + at + ")))";
    }
}

// a byte nothing folds: the harness's I/O reads give the same values on both sides
std::string Gen::StretchInput() {
    return "(io_read u8 " + std::to_string(0xC000 + stretch_rng.Below(256)) + ")";
}

// a helper big enough to stay a call: (sbigN) or (sbigN k)
std::string Gen::StretchBig(int n) {
    Rng& r = stretch_rng;
    bool param = n % 2 == 1;
    std::string text = "(fn sbig" + Num(n) + " (" + (param ? "(k u8)" : "") + ") void";
    text += "\n    (set sg (+ sg " + StretchInput() + "))";
    int count = 3 + r.Below(6);
    for (int i = 0; i < count; i++) {
        text += "\n    " + StretchWork(param ? "k" : "");
    }
    return text + ")\n";
}

std::string Gen::StretchCall(int n, const std::string& arg) {
    return "(sbig" + Num(n) + (n % 2 == 1 ? " " + arg : "") + ")";
}

// a helper whose last act is a call of sbig n, in one of four ways; padded with statements
// that make it too big to copy, or not
std::string Gen::StretchTail(int t, int n) {
    Rng& r = stretch_rng;
    std::string pad;
    int count = r.Chance(25) ? 0 : 3 + r.Below(5);
    for (int i = 0; i < count; i++) {
        pad += " " + StretchWork("c");
    }
    std::string k = Num(r.Below(256));
    std::string head = "(fn stail" + Num(t) + " ((c u8)) void";
    switch (r.Below(4)) {
    case 0:
        return head + pad + " (if (< c " + k + ") " + StretchCall(n, "c") + "))\n";
    case 1:
        return head + " (if (= (& c 3) " + Num(r.Below(4)) + ") (do " + StretchCall(n, "c") +
               " (return)))" + pad + " (set (at sbuf 7) c))\n";
    case 2:
        return head + pad + " (if (> c " + k + ") " + StretchCall(n, "c") + " " +
               StretchCall(n ^ 1, "(+ c 1)") + "))\n";
    default:
        return head + " (set sg (+ sg c))" + pad + " (if (!= c " + k + ") " +
               StretchCall(n, "(^ c sg)") + "))\n";
    }
}

std::string Gen::StretchHelpers() {
    Rng& r = stretch_rng;
    std::string text = "(let sg u8 " + Num(r.Below(256)) + ")\n(let sw u16 0)\n";
    text += "(let sbuf (array u8 64))\n";
    // the cuts: two reserved bytes each, the first past the startup
    int at = 0x6000 + 0xA0 + r.Below(0x80);
    int cuts = 6 + r.Below(14);
    for (int c = 0; c < cuts; c++) {
        text += "(let scut" + Num(c) + " (array u8 2) :address " + Num(at) + " :reserve)\n";
        at += 2 + 0x10 + r.Below(0x70);
    }
    // each tail helper next to its callee, before or after it, so that one layout of all the
    // code puts the branch in reach of it
    for (int t = 0; t < 6; t++) {
        if (t % 2 == 0) {
            text += StretchBig(t) + StretchTail(t, t);
        } else {
            text += StretchTail(t, t) + StretchBig(t);
        }
    }
    // every helper called from two places at least, so none is copied for being called once
    text += "(fn srun ((x u8)) void";
    for (int t = 0; t < 6; t++) {
        text += "\n    (stail" + Num(t) + " (+ x " + StretchInput() + "))";
        text += "\n    (stail" + Num(t) + " " + StretchInput() + ")";
        text += "\n    " + StretchCall(t, "x");
    }
    return text + ")\n";
}

std::string Gen::StretchShape(int depth) {
    Rng& r = stretch_rng;
    std::string arg =
        r.Chance(60) ? StretchInput() : "(^ " + StretchInput() + " " + Expr(T::U8, 1) + ")";
    switch (r.Below(6)) {
    case 0:
        return StretchCall(r.Below(6), arg);
    case 1:
    case 2:
        return "(srun " + arg + ")";
    case 5:
        if (depth > 0) {
            return "(if " + BoolExpr(1) + " (stail" + Num(r.Below(6)) + " " + arg + "))";
        }
        return "(stail0 " + arg + ")";
    default:
        return "(stail" + Num(r.Below(6)) + " " + arg + ")";
    }
}

} // namespace tisple_tests::fuzz
