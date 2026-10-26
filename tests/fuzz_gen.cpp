#include "fuzz_gen.hpp"

#include <vector>

namespace tisple_tests {

namespace {

enum class T : std::uint8_t { U8, I8, U16, I16, Bool, Enum };

const T kInts[] = {T::U8, T::I8, T::U16, T::I16};

const char* Name(T t) {
    switch (t) {
    case T::U8:
        return "u8";
    case T::I8:
        return "i8";
    case T::U16:
        return "u16";
    case T::I16:
        return "i16";
    case T::Bool:
        return "bool";
    default:
        return "E";
    }
}

bool IsInt(T t) {
    return t == T::U8 || t == T::I8 || t == T::U16 || t == T::I16;
}

struct Rng {
    std::uint64_t s = 1;

    std::uint32_t Next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<std::uint32_t>(s >> 11);
    }
    int Below(int n) {
        return n <= 0 ? 0 : static_cast<int>(Next() % static_cast<std::uint32_t>(n));
    }
    bool Chance(int percent) {
        return Below(100) < percent;
    }
};

struct VarInfo {
    std::string name;
    T type = T::U8;
    bool read_only = false;
};

struct ArrayInfo {
    std::string name;
    T elem = T::U8;
    std::uint32_t count = 0;
    bool is_const = false;
    bool pointers = false; // addr_of reaches into it
};

struct FnInfo {
    std::string name;
    std::vector<T> params;
    bool has_ret = false;
    T ret = T::U8;
};

struct PtrInfo {
    std::string name;
    T elem = T::U8;
    bool to_struct = false;
    std::size_t depth = 0; // the scope it was declared in
};

// struct S (a u8) (b i16) (c bool) (d u8)
const char* const kFields[] = {"a", "b", "c", "d"};
const T kFieldTypes[] = {T::U8, T::I16, T::Bool, T::U8};
constexpr int kEnumMembers = 5;

struct Gen {
    Rng rng;
    std::vector<VarInfo> globals;
    std::vector<ArrayInfo> arrays;
    std::vector<FnInfo> fns;
    std::size_t current = 0; // the function being written; calls go to earlier ones
    std::vector<std::vector<VarInfo>> scopes;
    std::vector<PtrInfo> ptrs; // pointer locals in scope (whole function)
    int names = 0;
    int loops = 0;
    // a u16 loop variable known to stay below 512, and a pointer into wb, inside its loop
    std::string wide;
    std::string wide_ptr;
    bool in_while = false;
    int budget = 0;

    std::string Fresh(const char* prefix) {
        return prefix + std::to_string(names++);
    }

    std::string Literal(T t) {
        switch (t) {
        case T::U8: {
            int pick = rng.Below(6);
            int v = pick == 0   ? 0
                    : pick == 1 ? 255
                    : pick == 2 ? 128
                                : rng.Below(pick == 3 ? 8 : 256);
            return std::to_string(v);
        }
        case T::I8:
            return std::to_string(rng.Chance(20) ? (rng.Chance(50) ? -128 : 127)
                                                 : rng.Below(256) - 128);
        case T::U16: {
            int pick = rng.Below(5);
            long v = pick == 0   ? 65535
                     : pick == 1 ? rng.Below(300)
                                 : static_cast<long>(rng.Next() & 0xFFFF);
            return std::to_string(v);
        }
        case T::I16:
            return std::to_string(rng.Chance(20) ? (rng.Chance(50) ? -32768 : 32767)
                                                 : static_cast<long>(rng.Next() & 0xFFFF) - 32768);
        case T::Bool:
            return rng.Chance(50) ? "true" : "false";
        default:
            return "E.e" + std::to_string(rng.Below(kEnumMembers));
        }
    }

    // variables of this type in scope: locals, params, globals
    std::vector<const VarInfo*> VarsOf(T t, bool writable) {
        std::vector<const VarInfo*> found;
        for (const auto& scope : scopes) {
            for (const VarInfo& v : scope) {
                if (v.type == t && (!writable || !v.read_only)) {
                    found.push_back(&v);
                }
            }
        }
        for (const VarInfo& v : globals) {
            if (v.type == t) {
                found.push_back(&v);
            }
        }
        return found;
    }

    std::vector<const ArrayInfo*> ArraysOf(T t, bool writable) {
        std::vector<const ArrayInfo*> found;
        for (const ArrayInfo& a : arrays) {
            if (a.elem == t && (!writable || !a.is_const)) {
                found.push_back(&a);
            }
        }
        return found;
    }

    // an index that stays inside the array
    std::string Index(const ArrayInfo& a) {
        if (a.count > 256) {
            return "(& " + NonLit(T::U16, 1) + " " + std::to_string(a.count - 1) + ")";
        }
        if (rng.Chance(30)) {
            return std::to_string(rng.Below(static_cast<int>(a.count)));
        }
        if (a.count == 256) {
            return NonLit(T::U8, 1);
        }
        return "(& " + NonLit(T::U8, 1) + " " + std::to_string(a.count - 1) + ")";
    }

    std::string Var(T t) {
        std::vector<const VarInfo*> vars = VarsOf(t, false);
        return vars[static_cast<std::size_t>(rng.Below(static_cast<int>(vars.size())))]->name;
    }

    std::string Leaf(T t) {
        int pick = rng.Below(10);
        if (pick < 3) {
            return Literal(t);
        }
        if (pick < 8 || t == T::Enum) {
            return Var(t);
        }
        if (t == T::Bool || t == T::I16 || t == T::U8) {
            if (rng.Chance(50)) {
                return StructField(t);
            }
        }
        std::vector<const ArrayInfo*> arrs = ArraysOf(t, false);
        if (arrs.empty()) {
            return Var(t);
        }
        const ArrayInfo& a =
            *arrs[static_cast<std::size_t>(rng.Below(static_cast<int>(arrs.size())))];
        return "(at " + a.name + " " + std::to_string(rng.Below(static_cast<int>(a.count))) + ")";
    }

    // a field of type t: of an element of sa, or of gs
    std::string StructField(T t) {
        std::vector<int> fields;
        for (int f = 0; f < 4; f++) {
            if (kFieldTypes[f] == t) {
                fields.push_back(f);
            }
        }
        if (fields.empty()) {
            return Var(t);
        }
        const char* field =
            kFields[fields[static_cast<std::size_t>(rng.Below(static_cast<int>(fields.size())))]];
        if (rng.Chance(40)) {
            return std::string("gs.") + field;
        }
        return "(. (at sa (& " + NonLit(T::U8, 0) + " 7)) " + field + ")";
    }

    // never a bare literal: those need a type from their context
    std::string NonLit(T t, int depth) {
        for (int tries = 0; tries < 4; tries++) {
            std::string e = Expr(t, depth);
            // an if or match may get its type only from literal arms
            bool untyped = e.rfind("(if ", 0) == 0 || e.rfind("(match ", 0) == 0;
            bool name = e[0] >= 'a' && e[0] <= 'z' && e != "true" && e != "false";
            if ((e[0] == '(' && !untyped) || name) {
                return e;
            }
        }
        return Var(t);
    }

    std::string Count() {
        if (rng.Chance(50)) {
            return std::to_string(rng.Below(18));
        }
        return "(& " + NonLit(T::U8, 1) + " 15)";
    }

    std::string Call(T t, int depth) {
        std::vector<std::size_t> callees;
        for (std::size_t f = 0; f < current; f++) {
            if (fns[f].has_ret && fns[f].ret == t) {
                callees.push_back(f);
            }
        }
        if (callees.empty() || depth <= 0) {
            return "";
        }
        return CallText(
            fns[callees[static_cast<std::size_t>(rng.Below(static_cast<int>(callees.size())))]],
            depth);
    }

    std::string CallText(const FnInfo& fn, int depth) {
        std::string text = "(" + fn.name;
        for (T p : fn.params) {
            text += " " + Expr(p, depth - 1);
        }
        return text + ")";
    }

    std::string Pointer(T t) {
        std::vector<const PtrInfo*> found;
        for (const PtrInfo& p : ptrs) {
            if (!p.to_struct && p.elem == t) {
                found.push_back(&p);
            }
        }
        if (found.empty()) {
            return "";
        }
        return found[static_cast<std::size_t>(rng.Below(static_cast<int>(found.size())))]->name;
    }

    std::string PointerRead(T t, int depth) {
        if (t == T::I16 || t == T::U8) {
            for (const PtrInfo& p : ptrs) {
                if (p.to_struct && rng.Chance(50)) {
                    return std::string("(load ") + p.name + " " + (t == T::I16 ? "b" : "d") + ")";
                }
            }
        }
        std::string p = Pointer(t);
        if (p.empty()) {
            return "";
        }
        if (rng.Chance(50)) {
            return "(load " + p + ")";
        }
        return "(load (+ " + p + " (u16 (& " + NonLit(T::U8, depth - 1) + " 3))))";
    }

    std::string IntExpr(T t, int depth) {
        static const char* const arith[] = {"+", "-", "*", "&", "|", "^", "+", "-"};
        if (!wide.empty() && (t == T::U8 || t == T::U16) && rng.Chance(20)) {
            if (t == T::U16) {
                return rng.Chance(50) ? "(at ww " + wide + ")" : wide;
            }
            return rng.Chance(50) ? "(at wb " + wide + ")"
                                  : "(load (+ " + wide_ptr + " " + wide + "))";
        }
        int pick = rng.Below(14);
        switch (pick) {
        case 0:
        case 1:
        case 2:
            return std::string("(") + arith[rng.Below(8)] + " " + NonLit(t, depth - 1) + " " +
                   Expr(t, depth - 1) + ")";
        case 3:
            return std::string("(") + (rng.Chance(50) ? "<<" : ">>") + " " + NonLit(t, depth - 1) +
                   " " + Count() + ")";
        case 4:
            return std::string("(") + (rng.Chance(50) ? "/" : "%") + " " + NonLit(t, depth - 1) +
                   " (| " + NonLit(t, depth - 1) + " 1))";
        case 5: {
            T from = kInts[rng.Below(4)];
            if (rng.Chance(15) && t == T::U8) {
                return "(u8 " + NonLit(T::Enum, depth - 1) + ")";
            }
            return std::string("(") + Name(t) + " " + NonLit(from, depth - 1) + ")";
        }
        case 6:
            return "(~ " + NonLit(t, depth - 1) + ")";
        case 7:
            return "(if " + Expr(T::Bool, depth - 1) + " " + Expr(t, depth - 1) + " " +
                   Expr(t, depth - 1) + ")";
        case 8: {
            std::string call = Call(t, depth);
            return call.empty() ? Leaf(t) : call;
        }
        case 9: {
            std::vector<const ArrayInfo*> arrs = ArraysOf(t, false);
            if (arrs.empty()) {
                return Leaf(t);
            }
            const ArrayInfo& a =
                *arrs[static_cast<std::size_t>(rng.Below(static_cast<int>(arrs.size())))];
            return "(at " + a.name + " " + Index(a) + ")";
        }
        case 10:
            if (t == T::U8 || t == T::U16) {
                return std::string("(io_read ") + Name(t) + " " + IoAddress() + ")";
            }
            return Leaf(t);
        case 11: {
            std::string load = PointerRead(t, depth);
            return load.empty() ? Leaf(t) : load;
        }
        case 12:
            return "(match (& " + NonLit(T::U8, depth - 1) + " 3) (0 " + Expr(t, depth - 1) +
                   ") (2 " + Expr(t, depth - 1) + ") (_ " + Expr(t, depth - 1) + "))";
        default:
            return Leaf(t);
        }
    }

    std::string BoolExpr(int depth) {
        static const char* const compares[] = {"<", "<=", ">", ">=", "=", "!="};
        int pick = rng.Below(9);
        switch (pick) {
        case 0:
        case 1:
        case 2: {
            T t = kInts[rng.Below(4)];
            return std::string("(") + compares[rng.Below(6)] + " " + NonLit(t, depth - 1) + " " +
                   Expr(t, depth - 1) + ")";
        }
        case 3:
            return std::string("(") + (rng.Chance(50) ? "and" : "or") + " " +
                   Expr(T::Bool, depth - 1) + " " + Expr(T::Bool, depth - 1) + ")";
        case 4:
            return "(not " + Expr(T::Bool, depth - 1) + ")";
        case 5:
            return std::string("(") + (rng.Chance(50) ? "=" : "!=") + " " +
                   NonLit(T::Enum, depth - 1) + " " + Expr(T::Enum, depth - 1) + ")";
        case 6: {
            std::string call = Call(T::Bool, depth);
            return call.empty() ? Leaf(T::Bool) : call;
        }
        default:
            return Leaf(T::Bool);
        }
    }

    std::string Expr(T t, int depth) {
        if (depth <= 0 || rng.Chance(25)) {
            return Leaf(t);
        }
        if (t == T::Bool) {
            return BoolExpr(depth);
        }
        if (t == T::Enum) {
            if (rng.Chance(30)) {
                return "(if " + Expr(T::Bool, depth - 1) + " " + Literal(T::Enum) + " " +
                       Expr(T::Enum, depth - 1) + ")";
            }
            return Leaf(t);
        }
        return IntExpr(t, depth);
    }

    std::string IoAddress() {
        return std::to_string(0xC000 + rng.Below(256));
    }

    T AnyType() {
        int pick = rng.Below(10);
        if (pick < 8) {
            return kInts[pick % 4];
        }
        return pick == 8 ? T::Bool : T::Enum;
    }

    // ---- statements

    std::string Place(T& t) {
        for (int tries = 0; tries < 8; tries++) {
            t = AnyType();
            int pick = rng.Below(10);
            if (pick < 5) {
                std::vector<const VarInfo*> vars = VarsOf(t, true);
                if (!vars.empty()) {
                    return vars[static_cast<std::size_t>(rng.Below(static_cast<int>(vars.size())))]
                        ->name;
                }
            } else if (pick < 8) {
                std::vector<const ArrayInfo*> arrs = ArraysOf(t, true);
                if (!arrs.empty()) {
                    const ArrayInfo& a =
                        *arrs[static_cast<std::size_t>(rng.Below(static_cast<int>(arrs.size())))];
                    return "(at " + a.name + " " + Index(a) + ")";
                }
            } else {
                for (int f = 0; f < 4; f++) {
                    if (kFieldTypes[f] == t) {
                        return StructField(t);
                    }
                }
            }
        }
        t = T::U8;
        return globals[0].name;
    }

    // pointers declared in scopes that ended
    void DropPointers() {
        while (!ptrs.empty() && ptrs.back().depth > scopes.size()) {
            ptrs.pop_back();
        }
    }

    std::string Body(int depth, int count) {
        scopes.emplace_back();
        std::string text;
        for (int i = 0; i < count && budget > 0; i++) {
            text += " " + Stmt(depth);
        }
        scopes.pop_back();
        DropPointers();
        return text.empty() ? " (do)" : text;
    }

    // statements as one form with no value, so arms never have to agree on a type
    std::string Arm(int depth, int count) {
        return " (do" + Body(depth, count) + " (do))";
    }

    std::string LoopBody(int depth, bool is_while) {
        bool was_while = in_while;
        in_while = is_while;
        loops++;
        std::string body = Body(depth - 1, 1 + rng.Below(3));
        loops--;
        in_while = was_while;
        return body;
    }

    std::string For(int depth) {
        T t = kInts[rng.Below(4)];
        std::string i = Fresh("n");
        std::string start;
        std::string end;
        bool is_signed = t == T::I8 || t == T::I16;
        int lo = is_signed ? rng.Below(8) - 4 : rng.Below(4);
        start = std::to_string(lo);
        if (rng.Chance(50)) {
            end = std::to_string(lo + rng.Below(8));
        } else if (t == T::U8) {
            end = "(& " + NonLit(T::U8, 1) + " 7)";
        } else {
            end = std::string("(") + Name(t) + " (& " + NonLit(T::U8, 1) + " 7))";
        }
        if (rng.Chance(20) && t == T::U8) {
            start = "(& " + NonLit(T::U8, 1) + " 3)";
        }
        if (rng.Chance(5) && t == T::U8) {
            start = "250";
            end = "256";
        }
        bool small = true;
        if (t == T::U16) {
            static const char* const kRanges[][2] = {
                {"250", "262"}, {"250", "256"}, {"500", "512"}, {"254", "258"}, {"65533", "65536"}};
            int pick = rng.Below(9);
            if (pick < 5) {
                start = kRanges[pick][0];
                end = kRanges[pick][1];
                small = pick != 4;
            }
        }
        std::string head = "(for " + i + " " + Name(t) + " " + start + " " + end;
        std::string outer_wide = wide;
        std::string outer_ptr = wide_ptr;
        std::string prefix;
        if (t == T::U16 && small && rng.Chance(70)) {
            std::string offset = NonLit(T::U8, 1);
            wide = i;
            wide_ptr = Fresh("wp");
            prefix = "(do (let " + wide_ptr + " (addr_of (at wb (u16 (& " + offset + " 15))))) ";
        }
        scopes.emplace_back();
        scopes.back().push_back(VarInfo{i, t, true});
        std::string body = LoopBody(depth, false);
        wide = outer_wide;
        wide_ptr = outer_ptr;
        scopes.pop_back();
        DropPointers();
        return prefix + head + body + ")" + (prefix.empty() ? "" : ")");
    }

    std::string While(int depth) {
        std::string c = Fresh("c");
        std::string text = "(do (let " + c + " u8 0) (while (< " + c + " " +
                           std::to_string(1 + rng.Below(5)) + ") (set " + c + " (+ " + c + " 1))";
        scopes.emplace_back();
        scopes.back().push_back(VarInfo{c, T::U8, true});
        text += LoopBody(depth, true) + "))";
        scopes.pop_back();
        DropPointers();
        return text;
    }

    std::string Match(int depth) {
        if (rng.Chance(40)) {
            std::string text = "(match " + NonLit(T::Enum, 1);
            for (int m = 0; m < kEnumMembers; m++) {
                text += " (E.e" + std::to_string(m) + Arm(depth - 1, 1) + ")";
            }
            return text + ")";
        }
        bool dense = rng.Chance(50);
        std::string text = "(match " + NonLit(T::U8, 1);
        int clauses = 2 + rng.Below(6);
        int value = rng.Below(20);
        for (int c = 0; c < clauses; c++) {
            text += " (" + std::to_string(value) + Arm(depth - 1, 1) + ")";
            value += dense ? 1 : 1 + rng.Below(40);
            if (value > 255) {
                break;
            }
        }
        return text + " (_" + Arm(depth - 1, 1) + "))";
    }

    std::string PointerLet() {
        std::vector<const ArrayInfo*> arrs;
        for (const ArrayInfo& a : arrays) {
            if (a.pointers) {
                arrs.push_back(&a);
            }
        }
        std::string p = Fresh("p");
        std::string text;
        if (rng.Chance(30)) {
            text = "(let " + p + " (addr_of (at sp (& " + NonLit(T::U8, 1) + " 3))))";
            ptrs.push_back(PtrInfo{p, T::U8, true, scopes.size()});
            return text;
        }
        const ArrayInfo& a =
            *arrs[static_cast<std::size_t>(rng.Below(static_cast<int>(arrs.size())))];
        std::string index = "(& " + NonLit(T::U8, 1) + " " + std::to_string(a.count / 2 - 1) + ")";
        if (a.count > 256) {
            index = "(u16 " + index + ")";
        }
        text = "(let " + p + " (addr_of (at " + a.name + " " + index + ")))";
        ptrs.push_back(PtrInfo{p, a.elem, false, scopes.size()});
        return text;
    }

    std::string Store() {
        for (const PtrInfo& p : ptrs) {
            if (p.to_struct && rng.Chance(40)) {
                int f = rng.Below(4);
                if (rng.Chance(25)) {
                    return "(store " + p.name + " (S :a " + Expr(T::U8, 1) + " :b " +
                           Expr(T::I16, 1) + " :c " + Expr(T::Bool, 1) + " :d " + Expr(T::U8, 1) +
                           "))";
                }
                return std::string("(store ") + p.name + " " + kFields[f] + " " +
                       Expr(kFieldTypes[f], 2) + ")";
            }
        }
        for (const PtrInfo& p : ptrs) {
            if (!p.to_struct && rng.Chance(60)) {
                std::string target =
                    rng.Chance(50) ? p.name
                                   : "(+ " + p.name + " (u16 (& " + NonLit(T::U8, 1) + " 3)))";
                return "(store " + target + " " + Expr(p.elem, 2) + ")";
            }
        }
        return "";
    }

    std::string WideStmt() {
        switch (rng.Below(3)) {
        case 0:
            return "(set (at ww " + wide + ") " + Expr(T::U16, 2) + ")";
        case 1:
            return "(set (at wb " + wide + ") " + Expr(T::U8, 2) + ")";
        default:
            return "(store (+ " + wide_ptr + " " + wide + ") " + Expr(T::U8, 2) + ")";
        }
    }

    std::string Stmt(int depth) {
        budget--;
        if (!wide.empty() && rng.Chance(35)) {
            return WideStmt();
        }
        int pick = rng.Below(depth > 0 ? 16 : 6);
        switch (pick) {
        case 0:
        case 1: {
            T t = AnyType();
            std::string name = Fresh("l");
            std::string text = std::string("(let ") + name + " " + Name(t) + " " + Expr(t, 3) + ")";
            scopes.back().push_back(VarInfo{name, t, false});
            return text;
        }
        case 2:
        case 3:
        case 4: {
            T t = T::U8;
            std::string place = Place(t);
            return "(set " + place + " " + Expr(t, 3) + ")";
        }
        case 5: {
            std::string store = Store();
            if (!store.empty()) {
                return store;
            }
            if (rng.Chance(50)) {
                return "(io_write u8 " + IoAddress() + " " + Expr(T::U8, 2) + ")";
            }
            return PointerLet();
        }
        case 6:
        case 7:
            if (rng.Chance(50)) {
                return "(if " + Expr(T::Bool, 2) + Arm(depth - 1, 1 + rng.Below(2)) + ")";
            }
            return "(if " + Expr(T::Bool, 2) + Arm(depth - 1, 1 + rng.Below(2)) +
                   Arm(depth - 1, 1 + rng.Below(2)) + ")";
        case 8:
        case 9:
            return For(depth);
        case 10:
            return While(depth);
        case 11:
            return Match(depth);
        case 12: {
            if (current == 0) {
                return "(io_read u8 " + IoAddress() + ")";
            }
            return CallText(fns[static_cast<std::size_t>(rng.Below(static_cast<int>(current)))], 2);
        }
        case 13:
            if (loops > 0) {
                const char* exit = rng.Chance(50) || in_while ? "(break)" : "(continue)";
                return "(if " + Expr(T::Bool, 2) + " " + exit + ")";
            }
            return PointerLet();
        case 14: {
            const FnInfo& fn = fns[current];
            std::string value = fn.has_ret ? " " + Expr(fn.ret, 2) : "";
            return "(if " + Expr(T::Bool, 2) + " (return" + value + "))";
        }
        default: {
            std::vector<const VarInfo*> vars = VarsOf(T::U16, true);
            const std::string& v =
                vars[static_cast<std::size_t>(rng.Below(static_cast<int>(vars.size())))]->name;
            return "(set " + v + " (+ " + v + " 1))";
        }
        }
    }

    std::string Function(std::size_t index) {
        current = index;
        const FnInfo& fn = fns[index];
        scopes.clear();
        ptrs.clear();
        scopes.emplace_back();
        std::string text = "(fn " + fn.name;
        if (!fn.params.empty()) {
            text += " (args";
            for (std::size_t p = 0; p < fn.params.size(); p++) {
                std::string name = Fresh("a");
                text += " (" + name + " " + Name(fn.params[p]) + ")";
                scopes.back().push_back(VarInfo{name, fn.params[p], false});
            }
            text += ")";
        }
        if (fn.has_ret) {
            text += std::string(" (returns ") + Name(fn.ret) + ")";
        }
        budget = 6 + rng.Below(14);
        while (budget > 0) {
            text += "\n    " + Stmt(3);
        }
        if (fn.has_ret) {
            text += "\n    " + Expr(fn.ret, 3);
        }
        return text + ")\n";
    }

    std::string Program() {
        std::string text = "(struct S (a u8) (b i16) (c bool) (d u8))\n(enum E";
        for (int m = 0; m < kEnumMembers; m++) {
            text += " e" + std::to_string(m);
        }
        text += ")\n";
        for (T t : {T::U8, T::I8, T::U16, T::I16, T::Bool, T::Enum}) {
            int count = 1 + rng.Below(3);
            for (int i = 0; i < count; i++) {
                std::string name = Fresh("g");
                std::string init = rng.Chance(70) ? " " + Literal(t) : "";
                text += std::string("(let ") + name + " " + Name(t) + init + ")\n";
                globals.push_back(VarInfo{name, t, false});
            }
        }
        static const std::uint32_t kCounts[] = {4, 8, 16, 32, 256, 512};
        int count = 3 + rng.Below(4);
        for (int i = 0; i < count; i++) {
            ArrayInfo a;
            a.name = Fresh("arr");
            a.elem = static_cast<T>(rng.Below(5));
            a.count = kCounts[rng.Below(6)];
            a.pointers = IsInt(a.elem) && a.count >= 8 && rng.Chance(40);
            text += "(let " + a.name + " (array " + Name(a.elem) + " " + std::to_string(a.count) +
                    "))\n";
            arrays.push_back(a);
        }
        // one array of pointer targets always exists
        ArrayInfo target;
        target.name = "pt";
        target.elem = T::U8;
        target.count = 16;
        target.pointers = true;
        text += "(let pt (array u8 16))\n(let wb (array u8 1024))\n(let ww (array u16 512))\n";
        arrays.push_back(target);
        ArrayInfo table;
        table.name = "KT";
        table.elem = T::U16;
        table.count = 8;
        table.is_const = true;
        text += "(const KT (array u16 8)";
        for (int i = 0; i < 8; i++) {
            text += " " + Literal(T::U16);
        }
        text += ")\n";
        arrays.push_back(table);
        text += "(let sa (array S 8))\n(let sp (array S 4))\n";
        text += "(let gs (S :a " + Literal(T::U8) + " :b " + Literal(T::I16) + " :c " +
                Literal(T::Bool) + " :d " + Literal(T::U8) + "))\n";

        int fn_count = 2 + rng.Below(4);
        for (int f = 0; f < fn_count; f++) {
            FnInfo fn;
            bool is_main = f == fn_count - 1;
            fn.name = is_main ? "main" : "f" + std::to_string(f);
            int params = is_main ? 0 : rng.Below(4);
            for (int p = 0; p < params; p++) {
                fn.params.push_back(AnyType());
            }
            fn.has_ret = is_main || rng.Chance(70);
            fn.ret = AnyType();
            fns.push_back(fn);
        }
        for (std::size_t f = 0; f < fns.size(); f++) {
            text += Function(f);
        }
        return text;
    }
};

} // namespace

std::string GenerateProgram(std::uint64_t seed) {
    Gen g;
    g.rng.s = seed * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    if (g.rng.s == 0) {
        g.rng.s = 1;
    }
    for (int i = 0; i < 4; i++) {
        g.rng.Next();
    }
    return g.Program();
}

} // namespace tisple_tests
