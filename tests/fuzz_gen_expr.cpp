#include "fuzz_gen_internal.hpp"

// expressions: literals, variables, operators, calls, loads, I/O

namespace tisple_tests::fuzz {

std::string Gen::Fresh(const char* prefix) {
    return prefix + std::to_string(names++);
}

std::string Gen::Literal(T t) {
    switch (t) {
    case T::U8: {
        int pick = rng.Below(6);
        int v = pick == 0 ? 0 : pick == 1 ? 255 : pick == 2 ? 128 : rng.Below(pick == 3 ? 8 : 256);
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
std::vector<const VarInfo*> Gen::VarsOf(T t, bool writable) {
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

std::vector<const ArrayInfo*> Gen::ArraysOf(T t, bool writable) {
    std::vector<const ArrayInfo*> found;
    for (const ArrayInfo& a : arrays) {
        if (a.elem == t && (!writable || !a.is_const)) {
            found.push_back(&a);
        }
    }
    return found;
}

// an index that stays inside the array
std::string Gen::Index(const ArrayInfo& a) {
    if (a.count > 256) {
        return "(& " + NonLit(T::U16, 1) + " " + std::to_string(a.count - 1) + ")";
    }
    if (rng.Chance(30)) {
        return std::to_string(rng.Below(static_cast<int>(a.count)));
    }
    if (a.count == 256) {
        return rng.Chance(40) ? ByteForm() : NonLit(T::U8, 1);
    }
    return "(& " + NonLit(T::U8, 1) + " " + std::to_string(a.count - 1) + ")";
}

std::string Gen::Var(T t) {
    std::vector<const VarInfo*> vars = VarsOf(t, false);
    return vars[static_cast<std::size_t>(rng.Below(static_cast<int>(vars.size())))]->name;
}

std::string Gen::Leaf(T t) {
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
    const ArrayInfo& a = *arrs[static_cast<std::size_t>(rng.Below(static_cast<int>(arrs.size())))];
    return "(at " + a.name + " " + std::to_string(rng.Below(static_cast<int>(a.count))) + ")";
}

// a field of type t: of an element of sa, or of gs
std::string Gen::StructField(T t) {
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
    return "(at sa (& " + NonLit(T::U8, 0) + " 7) " + field + ")";
}

// never a bare literal: those need a type from their context
std::string Gen::NonLit(T t, int depth) {
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

std::string Gen::Count() {
    if (rng.Chance(50)) {
        return std::to_string(rng.Below(18));
    }
    return "(& " + NonLit(T::U8, 1) + " 15)";
}

std::string Gen::Call(T t, int depth) {
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
        fns[callees[static_cast<std::size_t>(rng.Below(static_cast<int>(callees.size())))]], depth);
}

std::string Gen::CallText(const FnInfo& fn, int depth) {
    std::string text = "(" + fn.name;
    for (T p : fn.params) {
        text += " " + Expr(p, depth - 1);
    }
    return text + ")";
}

const PtrInfo* Gen::Pointer(T t) {
    std::vector<const PtrInfo*> found;
    for (const PtrInfo& p : ptrs) {
        if (!p.to_struct && p.elem == t) {
            found.push_back(&p);
        }
    }
    if (found.empty()) {
        return nullptr;
    }
    return found[static_cast<std::size_t>(rng.Below(static_cast<int>(found.size())))];
}

std::string Gen::PointerCount(const PtrInfo& p, int depth) {
    if (p.room) {
        return ByteIndex(depth);
    }
    return "(as u16 (& " + NonLit(T::U8, depth - 1) + " 3))";
}

std::string Gen::ByteForm() {
    std::string v = !counter.empty() && rng.Chance(50) ? counter : Var(T::U8);
    if (rng.Chance(15)) {
        // a byte that stores through pointers into wb can change under a kept index
        v = "(at wb " + std::to_string(rng.Below(4)) + ")";
    } else if (rng.Chance(10)) {
        // an element of a byte array of 256, which may index that same array, and a store
        // into it then changes the index
        for (const ArrayInfo& a : arrays) {
            if (a.elem == T::U8 && a.count == 256 && !a.is_const) {
                v = "(at " + a.name + " " + std::to_string(rng.Below(4)) + ")";
                break;
            }
        }
    }
    static const char* const kDeltas[] = {"1", "1", "2", "3", "200"};
    std::string k = kDeltas[rng.Below(5)];
    switch (rng.Below(5)) {
    case 0:
    case 1:
        return v;
    case 2:
        return "(+ " + v + " " + k + ")";
    case 3:
        return "(- " + v + " " + k + ")";
    default:
        return "(+ " + k + " " + v + ")";
    }
}

std::string Gen::ByteIndex(int depth) {
    int pick = rng.Below(10);
    if (pick < 2) {
        std::vector<std::string> lets;
        for (const auto& scope : scopes) {
            for (const VarInfo& v : scope) {
                if (v.small) {
                    lets.push_back(v.name);
                }
            }
        }
        if (!lets.empty()) {
            return lets[static_cast<std::size_t>(rng.Below(static_cast<int>(lets.size())))];
        }
    }
    if (pick < 7) {
        // a u8 count is (as u16 count) written short: both spellings must compile the same
        std::string byte = ByteForm();
        return pick % 2 == 1 ? byte : "(as u16 " + byte + ")";
    }
    if (pick < 8) {
        return std::to_string(rng.Below(256));
    }
    return "(as u16 " + NonLit(T::U8, depth - 1) + ")";
}

std::string Gen::PointerRead(T t, int depth) {
    if (t == T::I16 || t == T::U8) {
        for (const PtrInfo& p : ptrs) {
            if (p.to_struct && rng.Chance(50)) {
                return std::string("(load ") + p.name + " " + (t == T::I16 ? "b" : "d") + ")";
            }
        }
    }
    const PtrInfo* p = Pointer(t);
    if (!p) {
        return "";
    }
    if (fold_shapes && side.Chance(25)) {
        return "(load " + ChainedPointer(*p, depth) + ")";
    }
    if (rng.Chance(p->room ? 20 : 50)) {
        return "(load " + p->name + ")";
    }
    return "(load (+ " + p->name + " " + PointerCount(*p, depth) + "))";
}

std::string Gen::SideLiteral(T t) {
    int pick = side.Below(4);
    std::int32_t v = static_cast<std::int32_t>(side.Next() & 0xFFFF);
    if (pick == 0) {
        v = side.Below(4);
    } else if (pick == 1) {
        static const std::int32_t kEdges[] = {1, 0xFFFF, 0x7F, 0x80, 0x7FFF, 0x8000};
        v = kEdges[side.Below(6)];
    }
    switch (t) {
    case T::U8:
        return std::to_string(v & 0xFF);
    case T::I8:
        return std::to_string(static_cast<std::int8_t>(v & 0xFF));
    case T::U16:
        return std::to_string(v);
    default:
        return std::to_string(static_cast<std::int16_t>(v));
    }
}

std::string Gen::ConstChain(T t, int depth) {
    std::string x = NonLit(t, depth - 1);
    std::string a = SideLiteral(t);
    std::string b = SideLiteral(t);
    switch (side.Below(10)) {
    case 0:
        return "(+ (+ " + x + " " + a + ") " + b + ")";
    case 1:
        return "(- (+ " + a + " " + x + ") " + b + ")";
    case 2:
        return "(- " + a + " (- " + x + " " + b + "))";
    case 3:
        return "(+ (- " + x + " " + a + ") (+ " + b + " " + NonLit(t, depth - 1) + "))";
    case 4:
        return "(* (* " + x + " " + a + ") " + b + ")";
    case 5:
        return side.Chance(50) ? "(* " + x + " 1)" : "(* 1 (+ " + x + " 0))";
    case 6: {
        static const char* const kOps[] = {"&", "|", "^"};
        std::string op = kOps[side.Below(3)];
        return "(" + op + " (" + op + " " + x + " " + a + ") " + b + ")";
    }
    case 7:
        return side.Chance(50) ? "(- " + x + " 0)" : "(<< (+ 0 " + x + ") 0)";
    case 8:
        // a product with 0 still runs what x does
        return "(+ (* (* " + x + " " + a + ") 0) " + NonLit(t, depth - 1) + ")";
    default:
        return "(+ (+ " + x + " " + a + ") (- " + b + " " + SideLiteral(t) + "))";
    }
}

std::string Gen::ChainedPointer(const PtrInfo& p, int depth) {
    if (p.room) {
        int k1 = side.Below(128);
        int k2 = side.Below(128);
        switch (side.Below(3)) {
        case 0:
            return "(+ (+ " + p.name + " " + std::to_string(k1) + ") " + std::to_string(k2) + ")";
        case 1:
            return "(+ (+ " + p.name + " (as u16 (& " + NonLit(T::U8, depth - 1) + " 127))) " +
                   std::to_string(k1) + ")";
        default:
            return "(- (+ " + p.name + " " + std::to_string(k1 + k2) + ") " + std::to_string(k2) +
                   ")";
        }
    }
    int k1 = side.Below(4);
    int k2 = side.Below(4 - k1);
    switch (side.Below(3)) {
    case 0:
        return "(+ (+ " + p.name + " " + std::to_string(k1) + ") " + std::to_string(k2) + ")";
    case 1:
        return "(- (+ " + p.name + " 3) " + std::to_string(k1) + ")";
    default:
        return "(+ (+ " + p.name + " 0) (as u16 (& " + NonLit(T::U8, depth - 1) + " 3)))";
    }
}

std::string Gen::IntExpr(T t, int depth) {
    static const char* const arith[] = {"+", "-", "*", "&", "|", "^", "+", "-"};
    if (!wide.empty() && (t == T::U8 || t == T::U16) && rng.Chance(20)) {
        if (t == T::U16) {
            return rng.Chance(50) ? "(at ww " + wide + ")" : wide;
        }
        return rng.Chance(50) ? "(at wb " + wide + ")" : "(load (+ " + wide_ptr + " " + wide + "))";
    }
    if (fold_shapes && side.Chance(12)) {
        return ConstChain(t, depth);
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
        // constant divisors, mostly odd ones, and now and then any, zero included, which has
        // a defined result
        if (rng.Chance(35)) {
            return std::string("(") + (rng.Chance(50) ? "/" : "%") + " " + NonLit(t, depth - 1) +
                   " " + Divisor(t) + ")";
        }
        if (rng.Chance(20)) {
            return std::string("(") + (rng.Chance(50) ? "/" : "%") + " " + NonLit(t, depth - 1) +
                   " " + Expr(t, depth - 1) + ")";
        }
        return std::string("(") + (rng.Chance(50) ? "/" : "%") + " " + NonLit(t, depth - 1) +
               " (| " + NonLit(t, depth - 1) + " 1))";
    case 5: {
        T from = kInts[rng.Below(4)];
        if (rng.Chance(15) && t == T::U8) {
            return "(as u8 " + NonLit(T::Enum, depth - 1) + ")";
        }
        return std::string("(as ") + Name(t) + " " + NonLit(from, depth - 1) + ")";
    }
    case 6:
        if (t == T::U16 && rng.Chance(40)) {
            return WideByte(depth);
        }
        return "(~ " + NonLit(t, depth - 1) + ")";
    case 7:
        return "(if " + Expr(T::Bool, depth - 1) + " " + Expr(t, depth - 1) + " " +
               Expr(t, depth - 1) + ")";
    case 8: {
        std::string call = Call(t, depth);
        return call.empty() ? Leaf(t) : call;
    }
    case 9: {
        if (t == T::U8 && rng.Chance(25)) {
            return SmallSum(depth);
        }
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
        const PtrInfo* p = t == T::U8 ? Pointer(T::U8) : nullptr;
        if (p && rng.Chance(25)) {
            return "(peek2 " + p->name + " " + Expr(T::U8, depth - 1) + ")";
        }
        std::string load = PointerRead(t, depth);
        return load.empty() ? Leaf(t) : load;
    }
    case 12:
        return "(match (& " + NonLit(T::U8, depth - 1) + " 3) (0 " + Expr(t, depth - 1) + ") (2 " +
               Expr(t, depth - 1) + ") (_ " + Expr(t, depth - 1) + "))";
    default:
        return Leaf(t);
    }
}

std::string Gen::Divisor(T t) {
    static const int kUnsigned[] = {3,  5,  6,   7,   9,   10,  12,  15,
                                    25, 40, 100, 127, 128, 129, 200, 255};
    static const int kWide[] = {256, 300, 1000, 4097};
    static const int kSigned[] = {3, 5, 7, 10, 100, 127, -3, -7, -100, -128};
    if (t == T::I8 || t == T::I16) {
        return std::to_string(kSigned[rng.Below(10)]);
    }
    if (t == T::U16 && rng.Chance(20)) {
        return std::to_string(kWide[rng.Below(4)]);
    }
    if (rng.Chance(10)) {
        return std::to_string(1 << rng.Below(8));
    }
    return std::to_string(kUnsigned[rng.Below(16)]);
}

std::string Gen::SmallSum(int depth) {
    auto at = [&]() {
        if (rng.Chance(50)) {
            return "(at sm " + std::to_string(rng.Below(4)) + ")";
        }
        return "(at sm (& " + NonLit(T::U8, 0) + " 31))";
    };
    std::string last = rng.Chance(40) ? std::to_string(rng.Below(250)) : Expr(T::U8, depth - 1);
    return "(+ (+ " + at() + " " + at() + ") (+ " + at() + " " + last + "))";
}

std::string Gen::WideByte(int depth) {
    auto byte = [&]() {
        std::string x = NonLit(T::U8, depth - 1);
        switch (rng.Below(4)) {
        case 0:
            return "(as u16 (& " + x + " " + std::to_string(1 + rng.Below(15)) + "))";
        case 1:
            return "(as u16 (>> " + x + " " + std::to_string(1 + rng.Below(7)) + "))";
        case 2:
            return "(as u16 " + x + ")";
        default:
            return "(<< (as u16 " + x + ") " + std::to_string(rng.Below(16)) + ")";
        }
    };
    std::string a = byte();
    switch (rng.Below(4)) {
    case 0:
        return "(<< " + a + " " + std::to_string(rng.Below(16)) + ")";
    case 1:
        return "(* " + a + " " + std::to_string(1 + rng.Below(100)) + ")";
    case 2:
        return "(+ " + std::to_string(rng.Below(4) * 0x1000 + rng.Below(2) * 0xF0) + " " + a + ")";
    default:
        return "(+ " + a + " " + byte() + ")";
    }
}

std::string Gen::BoolExpr(int depth) {
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
        return std::string("(") + (rng.Chance(50) ? "and" : "or") + " " + Expr(T::Bool, depth - 1) +
               " " + Expr(T::Bool, depth - 1) + ")";
    case 4:
        return "(not " + Expr(T::Bool, depth - 1) + ")";
    case 5:
        return std::string("(") + (rng.Chance(50) ? "=" : "!=") + " " + NonLit(T::Enum, depth - 1) +
               " " + Expr(T::Enum, depth - 1) + ")";
    case 6: {
        std::string call = Call(T::Bool, depth);
        return call.empty() ? Leaf(T::Bool) : call;
    }
    default:
        return Leaf(T::Bool);
    }
}

std::string Gen::Expr(T t, int depth) {
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

std::string Gen::IoAddress() {
    return std::to_string(0xC000 + rng.Below(256));
}

T Gen::AnyType() {
    int pick = rng.Below(10);
    if (pick < 8) {
        return kInts[pick % 4];
    }
    return pick == 8 ? T::Bool : T::Enum;
}

} // namespace tisple_tests::fuzz
