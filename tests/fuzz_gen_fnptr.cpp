#include "fuzz_gen_internal.hpp"

// Function pointers (src/check/fnptr.cpp, src/backend/dispatch.cpp) and what the mid-level
// passes must assume of a function whose address is taken: a const table of targets indexed
// by computed values, a let array and struct fields of them set by finit (main calls it
// first) and reassigned, a global one called by name, pointers passed to fapply and returned
// by fpick, equality, calls in loops; targets that read and write the globals and arrays the
// callers use, some pure, also called directly with constants (parameter ranges and unused
// returns from the calls the tree shows would be wrong for the calls through pointers).

namespace tisple_tests::fuzz {

namespace {

const char* const kTa = "(fn (u8 u16) u8)";
const char* const kTb = "(fn (u8) void)";
const char* const kTc = "(fn () u16)";

std::string As(T to, T from, const std::string& x) {
    return to == from ? x : "(as " + std::string(Name(to)) + " " + x + ")";
}

} // namespace

std::string Gen::FnPtrVar(T t) {
    std::vector<const VarInfo*> vars = VarsOf(t, true);
    return vars[static_cast<std::size_t>(fnptr_rng.Below(static_cast<int>(vars.size())))]->name;
}

// a target's body: sets of globals and elements the callers use (none for a pure one), then
// its value, from its parameters, globals and arrays
std::string Gen::FnPtrBody(T ret, bool pure) {
    std::string text;
    if (!pure) {
        int sets = 1 + fnptr_rng.Below(3);
        for (int i = 0; i < sets; i++) {
            T t = kInts[fnptr_rng.Below(4)];
            std::string g = FnPtrVar(t);
            static const char* const kOps[] = {"+", "-", "^", "|"};
            text += "\n    (set " + g + " (" + kOps[fnptr_rng.Below(4)] + " " + g + " " +
                    Expr(t, 2) + "))";
        }
        if (fnptr_rng.Chance(50)) {
            text += "\n    (set (at fq (& " + Expr(T::U8, 1) + " 7)) " + Expr(T::U8, 2) + ")";
        }
    }
    if (ret == T::Bool) {
        return text;
    }
    return text + "\n    " + Expr(ret, 2);
}

std::string Gen::FnPtrHelpers() {
    fnptr_targets = true; // no calls through pointers in targets: that could be recursion
    std::size_t saved = current;
    current = 0; // targets call no generated function
    std::string text = "(let fq (array u8 8))\n";
    for (int k = 0; k < 3; k++) {
        scopes.clear();
        scopes.emplace_back();
        scopes.back().push_back(VarInfo{"x", T::U8, false});
        scopes.back().push_back(VarInfo{"y", T::U16, false});
        text += "(fn fa" + std::to_string(k) + " ((x u8) (y u16)) u8" +
                FnPtrBody(T::U8, k == 2 && fnptr_rng.Chance(50)) + ")\n";
        scopes.clear();
        scopes.emplace_back();
        scopes.back().push_back(VarInfo{"v", T::U8, false});
        text += "(fn fb" + std::to_string(k) + " ((v u8)) void" + FnPtrBody(T::Bool, false) + ")\n";
        scopes.clear();
        scopes.emplace_back();
        text += "(fn fc" + std::to_string(k) + " () u16" + FnPtrBody(T::U16, k != 0) + ")\n";
    }
    scopes.clear();
    current = saved;
    fnptr_targets = false;
    text += std::string("(const FTA (array ") + kTa + " 4) (make (array " + kTa +
            " 4) fa0 fa1 fa2 " + "fa" + std::to_string(fnptr_rng.Below(3)) + "))\n";
    text += std::string("(let fhb (array ") + kTb + " 4))\n";
    text += std::string("(let fgc ") + kTc + ")\n";
    text += std::string("(struct FO ((n u8) (f ") + kTb + ")))\n(let fos (array FO 4))\n";
    text += "(fn finit () void";
    for (int i = 0; i < 4; i++) {
        text += "\n    (set (at fhb " + std::to_string(i) + ") fb" +
                std::to_string(fnptr_rng.Below(3)) + ")";
    }
    text += "\n    (set fgc fc" + std::to_string(fnptr_rng.Below(3)) + ")";
    text += "\n    (for i u8 0 4 (set (at fos i f) (at fhb (- 3 i)))))\n";
    text += std::string("(fn fapply ((f ") + kTa + ") (a u8) (b u16)) u8 (+ (f a b) 1))\n";
    text += std::string("(fn fpick ((k u8)) ") + kTc + " (if (= (& k 1) 0) fc1 fc2))\n";
    return text;
}

// an index into a table of 4: a byte the caller works out, masked
std::string Gen::FnPtrIndex() {
    return "(& " + Expr(T::U8, 2) + " 3)";
}

// a call through a pointer, or a direct call of a target, that gives a u8 or u16 (in t)
std::string Gen::FnPtrValue(T t, std::string& lets) {
    std::string name = Fresh("fp");
    std::string call;
    switch (fnptr_rng.Below(7)) {
    case 0:
    case 1:
        lets += std::string("(let ") + name + " " + kTa + " (at FTA " + FnPtrIndex() + "))\n    ";
        call = "(" + name + " " + Expr(T::U8, 2) + " " + Expr(T::U16, 2) + ")";
        return As(t, T::U8, call);
    case 2:
        call = "(fapply " +
               std::string(fnptr_rng.Chance(50) ? "(at FTA " + FnPtrIndex() + ")"
                                                : "fa" + std::to_string(fnptr_rng.Below(3))) +
               " " + Expr(T::U8, 2) + " " + Expr(T::U16, 2) + ")";
        return As(t, T::U8, call);
    case 3:
        // the directly called targets mostly see constants
        call = "(fa" + std::to_string(fnptr_rng.Below(3)) + " " +
               std::to_string(fnptr_rng.Below(4)) + " " + std::to_string(fnptr_rng.Below(4)) + ")";
        return As(t, T::U8, call);
    case 4:
        return As(t, T::U16, "(fgc)");
    case 5:
        lets += std::string("(let ") + name + " " + kTc + " (fpick " + Expr(T::U8, 2) + "))\n    ";
        return As(t, T::U16, "(" + name + ")");
    default: {
        // a call in an argument of another
        std::string inner = Fresh("fp");
        lets += std::string("(let ") + name + " " + kTa + " (at FTA " + FnPtrIndex() + "))\n    ";
        lets += std::string("(let ") + inner + " " + kTa + " (at FTA " + FnPtrIndex() + "))\n    ";
        call = "(" + name + " (" + inner + " " + Expr(T::U8, 1) + " " + Expr(T::U16, 1) + ") " +
               Expr(T::U16, 2) + ")";
        return As(t, T::U8, call);
    }
    }
}

std::string Gen::FnPtrShape(int depth) {
    std::string lets;
    switch (fnptr_rng.Below(8)) {
    case 0:
    case 1: {
        T t = fnptr_rng.Chance(50) ? T::U8 : T::U16;
        std::string g = FnPtrVar(t);
        std::string value = FnPtrValue(t, lets);
        return "(do\n    " + lets + "(set " + g + " (+ " + g + " " + value + ")))";
    }
    case 2: {
        // a call whose value nothing uses
        std::string value = FnPtrValue(T::U16, lets);
        return "(do\n    " + lets + value + ")";
    }
    case 3: {
        std::string name = Fresh("fh");
        std::string where = fnptr_rng.Chance(50) ? "(at fhb " + FnPtrIndex() + ")"
                                                 : "(at fos " + FnPtrIndex() + " f)";
        return std::string("(do\n    (let ") + name + " " + kTb + " " + where + ")\n    (" + name +
               " " + Expr(T::U8, 2) + "))";
    }
    case 4:
        if (fnptr_rng.Chance(50)) {
            return "(set (at fhb " + FnPtrIndex() + ") fb" + std::to_string(fnptr_rng.Below(3)) +
                   ")";
        }
        if (fnptr_rng.Chance(50)) {
            return "(set (at fos " + FnPtrIndex() + " f) (at fhb " + FnPtrIndex() + "))";
        }
        return "(set fgc " +
               std::string(fnptr_rng.Chance(50) ? "(fpick " + Expr(T::U8, 2) + ")"
                                                : "fc" + std::to_string(fnptr_rng.Below(3))) +
               ")";
    case 5: {
        std::string g = FnPtrVar(T::U8);
        return "(if (= (at FTA " + FnPtrIndex() + ") fa" + std::to_string(fnptr_rng.Below(3)) +
               ") (set " + g + " (+ " + g + " 1)))";
    }
    default: {
        // calls in a loop whose targets write what the loop reads
        std::string i = Fresh("fi");
        std::string n = std::to_string(1 + fnptr_rng.Below(6));
        T t = fnptr_rng.Chance(50) ? T::U8 : T::U16;
        std::string g = FnPtrVar(t);
        scopes.emplace_back();
        scopes.back().push_back(VarInfo{i, T::U8, true});
        std::string value = FnPtrValue(t, lets);
        std::string body = "(do\n    " + lets + "(set " + g + " (+ " + g + " " + value + ")))";
        if (depth > 1 && fnptr_rng.Chance(30)) {
            body += "\n    " + Stmt(depth - 1);
        }
        scopes.pop_back();
        DropPointers();
        return "(for " + i + " u8 0 " + n + "\n    " + body + ")";
    }
    }
}

} // namespace tisple_tests::fuzz
