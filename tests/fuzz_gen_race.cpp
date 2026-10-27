#include "fuzz_gen.hpp"
#include "fuzz_gen_internal.hpp"

// race mode: the same programs, minus what C cannot say the same way (I/O), and a tail that
// folds every global into one checksum. The generated main becomes t.run; the new main calls
// t.init, which sets every global's start value, then calls run, keeps its value in rv and
// calls t.checksum, which stores the checksum in chk at $BF00. races/random.py times from
// init's return to t.checksum's first instruction.
//
// No compiler may fold a start value, as no compiler may fold the kernel race's inputs: an
// initializer, or a global only ever set to one constant, is a constant to LLVM. So init
// sets each global to its value plus opaque, a u16 at $BC00 that the race writes as 0 when
// main is entered (and the interpreter's memory is 0 there): (set g0 (+ 128 (as u8 opaque))).

namespace tisple_tests::fuzz {

namespace {

// a value of type t as a u16 for the checksum: sign-extended for i8, 0 or 1 for a bool
std::string AsWord(T t, const std::string& x) {
    switch (t) {
    case T::U16:
        return x;
    case T::Bool:
        return "(if " + x + " 1 0)";
    case T::Enum:
        return "(as u16 (as u8 " + x + "))";
    default:
        return "(as u16 " + x + ")";
    }
}

std::string Zero(T t) {
    switch (t) {
    case T::Bool:
        return "false";
    case T::Enum:
        return "E.e0";
    default:
        return "0";
    }
}

// value (a literal of type t) as an expression no compiler can fold, with opaque = 0
std::string Opaque(T t, const std::string& value) {
    switch (t) {
    case T::Bool:
        return value == "true" ? "(= opaque 0)" : "(!= opaque 0)";
    case T::Enum:
        return "(if (= opaque 0) " + value + " E.e1)";
    case T::U16:
        return "(+ " + value + " opaque)";
    default:
        return "(+ " + value + " (as " + Name(t) + " opaque))";
    }
}

std::string MixLine(const std::string& value) {
    return "\n    (set s (mix s " + value + "))";
}

// (for ckN T 0 count: a u8 counter for arrays of up to 256, which index by a u8
std::string LoopHead(std::uint32_t count, int& loop, std::string& i) {
    i = "ck" + std::to_string(loop++);
    return "\n    (for " + i + (count > 256 ? " u16 0 " : " u8 0 ") + std::to_string(count);
}

// every element of an array, in order
std::string ArrayLoop(const std::string& name, T elem, std::uint32_t count, int& loop) {
    std::string i;
    std::string head = LoopHead(count, loop, i);
    return head + " (set s (mix s " + AsWord(elem, "(at " + name + " " + i + ")") + ")))";
}

// every field of every element of an array of S, element by element
std::string StructLoop(const std::string& name, std::uint32_t count, int& loop) {
    std::string i;
    std::string text = LoopHead(count, loop, i);
    for (int f = 0; f < 4; f++) {
        std::string field = "(at " + name + " " + i + " " + kFields[f] + ")";
        text += " (set s (mix s " + AsWord(kFieldTypes[f], field) + "))";
    }
    return text + ")";
}

} // namespace

std::string Gen::RaceStart(const std::string& name, T t, const std::string& init) {
    race_init +=
        "\n    (set " + name + " " + Opaque(t, init.empty() ? Zero(t) : init.substr(1)) + ")";
    return "";
}

std::string Gen::RaceArrayStart(const std::string& name, T elem, const std::string& init) {
    if (init.empty()) {
        return "";
    }
    // " (make (array t n) v0 v1 ...)": the values after the type, one word each
    std::size_t at = init.find(')') + 1;
    std::string values = init.substr(at, init.size() - at - 1);
    int index = 0;
    std::size_t start = 1;
    while (start <= values.size()) {
        std::size_t end = values.find(' ', start);
        if (end == std::string::npos) {
            end = values.size();
        }
        race_init += "\n    (set (at " + name + " " + std::to_string(index++) + ") " +
                     Opaque(elem, values.substr(start, end - start)) + ")";
        start = end + 1;
    }
    return "";
}

std::string Gen::RaceStructStart() {
    for (int f = 0; f < 4; f++) {
        std::string value = Literal(kFieldTypes[f]);
        race_init +=
            std::string("\n    (set gs.") + kFields[f] + " " + Opaque(kFieldTypes[f], value) + ")";
    }
    return "";
}

std::string Gen::RaceStore() {
    T t = T::U8;
    std::string place = Place(t);
    return "(set " + place + " " + Expr(t, 2) + ")";
}

std::string Gen::RaceTail() {
    T ret = fns.back().ret;
    std::string text = std::string("(let rv ") + Name(ret) + ")\n";
    text += "(let chk u16 :address 0xBF00)\n";
    text += "(let opaque u16 :address 0xBC00)\n";
    // every array gets an opaque store too, or one no code writes is all zeros to LLVM
    std::string touch;
    for (const ArrayInfo& a : arrays) {
        if (!a.is_const) {
            touch += "\n    (set (at " + a.name + " 0) " + Opaque(a.elem, Zero(a.elem)) + ")";
        }
    }
    for (const char* name : {"wb", "sm"}) {
        touch += std::string("\n    (set (at ") + name + " 0) " + Opaque(T::U8, "0") + ")";
    }
    touch += "\n    (set (at ww 0) " + Opaque(T::U16, "0") + ")";
    for (const char* name : {"sa", "sp"}) {
        touch += std::string("\n    (set (at ") + name + " 0 a) " + Opaque(T::U8, "0") + ")";
    }
    text += "(fn init () void" + touch + race_init + ")\n";
    text += "(fn mix ((s u16) (v u16)) u16 (+ (| (<< s 1) (>> s 15)) v))\n";
    text += "(fn checksum () void\n    (let s u16 0)";
    for (const VarInfo& v : globals) {
        text += MixLine(AsWord(v.type, v.name));
    }
    int loop = 0;
    for (const ArrayInfo& a : arrays) {
        if (!a.is_const) {
            text += ArrayLoop(a.name, a.elem, a.count, loop);
        }
    }
    text += ArrayLoop("wb", T::U8, 1024, loop);
    text += ArrayLoop("ww", T::U16, 512, loop);
    text += ArrayLoop("sm", T::U8, 32, loop);
    text += StructLoop("sa", 8, loop);
    text += StructLoop("sp", 4, loop);
    for (int f = 0; f < 4; f++) {
        text += MixLine(AsWord(kFieldTypes[f], std::string("gs.") + kFields[f]));
    }
    text += MixLine(AsWord(ret, "rv"));
    text += "\n    (set chk s))\n";
    text += "(fn main () void (init) (set rv (run)) (checksum))\n";
    return text;
}

} // namespace tisple_tests::fuzz
