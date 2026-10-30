#include "fuzz_gen_internal.hpp"

#include <algorithm>

// Static data (src/check/data.cpp, src/opt/const_data.cpp): const tables of structs with arrays
// inside them, strings and addresses the link fills in, read at constant and computed indexes,
// through pointers and through function pointers kept in them:
//
//   (struct DMv ((p u8) (w i16) (s (ptr u8))))
//   (struct DFoe ((hp u8) (tag E) (name (ptr u8)) (moves (array DMv 3)) (step (fn (u8) u8))
//                 (big u16)))
//   DM0 a struct const; DMOVES (array DMv 4) and DFOES (array DFoe 2 to 5), whose moves copy
//   DM0 and DMOVES' elements now and then; DNAMES (array (ptr u8) 4) of strings, a string
//   moved on and a name read from DFOES; DPTR into DMOVES, DGP into the let dglob; the let
//   dcur starts at an element of DMOVES
//
// Every index is masked or taken modulo into its array and every string read stays inside the
// string and its 0, so a program never reads outside its data; dglob is the only data written
// through a constant pointer (consts are never written).

namespace tisple_tests::fuzz {

namespace {

std::string Num(int n) {
    return std::to_string(n);
}

const char* const kChars[] = {"'A'", "'z'", "' '", "'\\n'", "'\\''", "'\\x41'", "'0'", "'#'"};

} // namespace

// a string literal of 1 to 6 capitals; its length into len
std::string Gen::DataString(int& len) {
    Rng& r = data_rng;
    len = 1 + r.Below(6);
    std::string text = "\"";
    for (int i = 0; i < len; i++) {
        text += static_cast<char>('A' + r.Below(6));
    }
    return text + "\"";
}

// a byte for a p field: a number or a char literal
std::string Gen::DataByte() {
    Rng& r = data_rng;
    if (r.Chance(30)) {
        return kChars[r.Below(8)];
    }
    return Num(r.Below(256));
}

// a DMv value: built, DM0, or an element of DMOVES (not inside DMOVES itself)
std::string Gen::DataMove(bool copies, int& min_len) {
    Rng& r = data_rng;
    if (copies && r.Chance(25)) {
        min_len = std::min(min_len, data_dm0_len);
        return "DM0";
    }
    if (copies && r.Chance(25)) {
        int k = r.Below(4);
        min_len = std::min(min_len, data_move_len[k]);
        return "(at DMOVES " + Num(k) + ")";
    }
    int len = 0;
    std::string s = DataString(len);
    min_len = std::min(min_len, len);
    return "(make DMv :p " + DataByte() + " :w " + Num(r.Below(601) - 300) + " :s " + s + ")";
}

std::string Gen::DataHelpers() {
    Rng& r = data_rng;
    std::string text = "(struct DMv ((p u8) (w i16) (s (ptr u8))))\n";
    text += "(struct DFoe ((hp u8) (tag E) (name (ptr u8)) (moves (array DMv 3)) "
            "(step (fn (u8) u8)) (big u16)))\n";
    text += "(fn dstep0 ((x u8)) u8 (+ x " + Num(r.Below(256)) + "))\n";
    text += "(fn dstep1 ((x u8)) u8 (^ x " + Num(r.Below(256)) + "))\n";
    text += "(let dglob (array u8 4))\n(let dacc u16 0)\n";
    int len = 0;
    text += "(const DM0 DMv (make DMv :p " + DataByte() + " :w " + Num(r.Below(601) - 300) +
            " :s " + DataString(len) + "))\n";
    data_dm0_len = len;
    text += "(const DMOVES (array DMv 4)\n    (make (array DMv 4)";
    for (int k = 0; k < 4; k++) {
        int min_len = 99;
        text += "\n        " + DataMove(false, min_len);
        data_move_len[k] = min_len;
    }
    text += "))\n";
    data_moves_min = std::min(std::min(data_move_len[0], data_move_len[1]),
                              std::min(data_move_len[2], data_move_len[3]));
    data_foes = 2 + r.Below(4);
    data_name_min = 99;
    data_foe_moves_min = 99;
    text += "(const DFOES (array DFoe " + Num(data_foes) + ")\n    (make (array DFoe " +
            Num(data_foes) + ")";
    for (int i = 0; i < data_foes; i++) {
        int name_len = 0;
        std::string name = DataString(name_len);
        data_name_min = std::min(data_name_min, name_len);
        std::string moves = "(make (array DMv 3)";
        for (int k = 0; k < 3; k++) {
            moves += " " + DataMove(true, data_foe_moves_min);
        }
        moves += ")";
        text += "\n        (make DFoe :hp " + Num(r.Below(40)) + " :tag E.e" +
                Num(r.Chance(70) ? 1 + r.Below(2) : r.Below(kEnumMembers)) + " :name " + name +
                " :moves " + moves + " :step dstep" + Num(r.Below(2)) + " :big " +
                Num(r.Below(65536)) + ")";
    }
    text += "))\n";
    // the names: two strings, one moved on by one, and a foe's name
    int a = 0;
    int b = 0;
    int c = 0;
    std::string sa = DataString(a);
    std::string sb = DataString(b);
    std::string sc = DataString(c);
    text += "(const DNAMES (array (ptr u8) 4) (make (array (ptr u8) 4) " + sa + " " + sb + " (+ " +
            sc + " 1) (at DFOES " + Num(r.Below(data_foes)) + " name)))\n";
    data_names_min = std::min(std::min(a, b), std::min(c - 1, data_name_min));
    text += "(const DPTR (ptr DMv) (addr_of (at DMOVES 2)))\n";
    text += "(const DGP (ptr u8) (addr_of (at dglob " + Num(1 + r.Below(3)) + ")))\n";
    text += "(let dcur (ptr DMv) (addr_of (at DMOVES " + Num(r.Below(4)) + ")))\n";
    return text;
}

// an index into DFOES: constant, or a byte worked out, taken modulo the count
std::string Gen::DataFoe() {
    Rng& r = data_rng;
    if (r.Chance(40)) {
        return Num(r.Below(data_foes));
    }
    return "(% " + Expr(T::U8, 1) + " " + Num(data_foes) + ")";
}

// an index below n: constant or masked (n a power of two)
std::string Gen::DataIndex(int n) {
    Rng& r = data_rng;
    if (r.Chance(40)) {
        return Num(r.Below(n));
    }
    return "(& " + Expr(T::U8, 1) + " " + Num(n - 1) + ")";
}

// an index below 3
std::string Gen::DataMoveIndex() {
    Rng& r = data_rng;
    if (r.Chance(40)) {
        return Num(r.Below(3));
    }
    return "(% " + Expr(T::U8, 1) + " 3)";
}

// a u16 from const data: fields, nested elements, strings, loads through constant pointers
std::string Gen::DataValue() {
    Rng& r = data_rng;
    switch (r.Below(12)) {
    case 0:
        return "(as u16 (at DFOES " + DataFoe() + " hp))";
    case 1:
        return "(at DFOES " + DataFoe() + " big)";
    case 2:
        return "(as u16 (at (at DFOES " + DataFoe() + " moves) " + DataMoveIndex() + " w))";
    case 3:
        return "(as u16 (at (at DFOES " + DataFoe() + " moves) " + DataMoveIndex() + " p))";
    case 4:
        return "(as u16 (load (+ (at (at DFOES " + DataFoe() + " moves) " + DataMoveIndex() +
               " s) " + Num(r.Below(data_foe_moves_min + 1)) + ")))";
    case 5:
        return "(as u16 (load (+ (at DNAMES " + DataIndex(4) + ") " +
               Num(r.Below(data_names_min + 1)) + ")))";
    case 6:
        return "(as u16 (load (+ (at DFOES " + DataFoe() + " name) " +
               Num(r.Below(data_name_min + 1)) + ")))";
    case 7: {
        // through DPTR, moved inside DMOVES (DPTR is its element 2)
        std::string p =
            r.Chance(50) ? "(+ DPTR " + Num(r.Below(2)) + ")" : "(- DPTR " + Num(r.Below(3)) + ")";
        return "(as u16 (load " + p + " " + (r.Chance(50) ? "w" : "p") + "))";
    }
    case 8:
        return "(as u16 (load (+ (load dcur s) " + Num(r.Below(data_moves_min + 1)) + ")))";
    case 9:
        return "(as u16 (load DM0.s))";
    case 10:
        return "(+ (as u16 DM0.p) (as u16 (at DMOVES " + DataIndex(4) + " w)))";
    default:
        return "(as u16 (at dglob " + DataIndex(4) + "))";
    }
}

std::string Gen::DataShape(int depth) {
    Rng& r = data_rng;
    switch (r.Below(9)) {
    case 0:
    case 1:
    case 2:
        return "(set dacc (+ dacc " + DataValue() + "))";
    case 3: {
        std::string f = Fresh("df");
        return "(do\n    (let " + f + " (fn (u8) u8) (at DFOES " + DataFoe() + " step))\n" +
               "    (set dacc (+ dacc (as u16 (" + f + " " + Expr(T::U8, 1) + ")))))";
    }
    case 4:
        return "(store (+ DGP " + Num(r.Below(1)) + ") " + Expr(T::U8, 1) + ")";
    case 5:
        return "(set dcur " +
               std::string(r.Chance(50) ? "DPTR" : "(addr_of (at DMOVES " + DataIndex(4) + "))") +
               ")";
    case 6:
        // the tags hold few values: compares and clauses the ranges pass may decide
        return "(if (= (at DFOES " + DataFoe() + " tag) E.e" + Num(r.Below(kEnumMembers)) +
               ") (set dacc (+ dacc 1)) (set dacc (- dacc 1)))";
    case 7: {
        std::string text = "(match (at DFOES " + DataFoe() + " tag)";
        for (int k = 0; k < 2; k++) {
            text +=
                "\n    (E.e" + Num(k * 2 + r.Below(2)) + " (set dacc (+ dacc " + Num(k + 2) + ")))";
        }
        return text + "\n    (_ (set dacc (^ dacc 5))))";
    }
    default: {
        // a move copied out of const data, and a pointer to one
        std::string m = Fresh("dm");
        std::string q = Fresh("dq");
        std::string text = "(do\n    (let " + m + " DMv (at (at DFOES " + DataFoe() + " moves) " +
                           DataMoveIndex() + "))\n";
        if (r.Chance(30)) {
            // an addr_of into DFOES: it is stored packed
            text += "    (let " + q + " (ptr DMv) (addr_of (at (at DFOES " + DataFoe() +
                    " moves) " + DataMoveIndex() + ")))\n";
            text += "    (set dacc (+ dacc (as u16 (load " + q + " w))))\n";
        }
        text += "    (set dacc (+ dacc (as u16 " + m + ".p))))";
        if (depth > 1 && r.Chance(30)) {
            // the statement's lets end with the do
            scopes.emplace_back();
            text = "(do " + text + " " + Stmt(depth - 1) + ")";
            scopes.pop_back();
            DropPointers();
        }
        return text;
    }
    }
}

} // namespace tisple_tests::fuzz
