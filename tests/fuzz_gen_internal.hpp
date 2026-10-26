#pragma once

// The fuzz generator's state, shared by its expression and statement halves.

#include <cstdint>
#include <string>
#include <vector>

namespace tisple_tests::fuzz {

enum class T : std::uint8_t { U8, I8, U16, I16, Bool, Enum };

inline const T kInts[] = {T::U8, T::I8, T::U16, T::I16};

inline const char* Name(T t) {
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

inline bool IsInt(T t) {
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
inline const char* const kFields[] = {"a", "b", "c", "d"};
inline const T kFieldTypes[] = {T::U8, T::I16, T::Bool, T::U8};
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

    std::string Fresh(const char* prefix);
    std::string Literal(T t);
    // variables of this type in scope: locals, params, globals
    std::vector<const VarInfo*> VarsOf(T t, bool writable);
    std::vector<const ArrayInfo*> ArraysOf(T t, bool writable);
    // an index that stays inside the array
    std::string Index(const ArrayInfo& a);
    std::string Var(T t);
    std::string Leaf(T t);
    // a field of type t: of an element of sa, or of gs
    std::string StructField(T t);
    // never a bare literal: those need a type from their context
    std::string NonLit(T t, int depth);
    std::string Count();
    std::string Call(T t, int depth);
    std::string CallText(const FnInfo& fn, int depth);
    std::string Pointer(T t);
    std::string PointerRead(T t, int depth);
    std::string IntExpr(T t, int depth);
    std::string BoolExpr(int depth);
    std::string Expr(T t, int depth);
    std::string IoAddress();
    T AnyType();
    // ---- statements
    std::string Place(T& t);
    // pointers declared in scopes that ended
    void DropPointers();
    std::string Body(int depth, int count);
    // statements as one form with no value, so arms never have to agree on a type
    std::string Arm(int depth, int count);
    std::string LoopBody(int depth, bool is_while);
    std::string For(int depth);
    std::string While(int depth);
    std::string Match(int depth);
    std::string PointerLet();
    std::string Store();
    std::string WideStmt();
    std::string Stmt(int depth);
    std::string Function(std::size_t index);
    std::string Program();
};

} // namespace tisple_tests::fuzz
