#pragma once

// The typed program: what the checker hands to the layout, the reference interpreter
// and the 6502 backend.
//
// - Flat arrays indexed by 32-bit ids (types, structs, enums, vars, functions, exprs);
//   -1 means none. Ids are stable once the checker returns.
// - Types are interned: two types are equal exactly when their TypeIds are equal.
// - Every name is resolved. A Var is any storage: a top-level let, a const array, a
//   parameter or a local (one Var per declaration; code refers to it by id, never by text).
//   Scalar constants and enum members are folded into Literal exprs.
// - Every Expr has its type. Statements are Void; return, break and continue are Never.
// - A function body is a Do. Calls name a Function id. Pointer arithmetic is PtrAdd/PtrSub
//   with the element size in bytes in `value`. Field keeps the field's byte offset in the
//   packed struct layout in `value`; the layout maps it to an address (see link/layout.hpp).
// - Places (set targets, addr_of operands) are VarRef, Index and Field chains rooted at a Var.
//   Memory behind a pointer is reached only by Load/Store and LoadField/StoreField (a constant
//   offset from the pointer: LDY #off / LDA (p),Y). A Load or LoadField may be the base of
//   Index when reading.
// - Struct values only appear as places, Load/LoadField, or StructLit (in set, let, store,
//   field values and top-level initializers). Arrays are never copied as values.
// - Initial values of Vars are bytes in packed order (fields in order, u16 low byte first).

#include "check/diag.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

using TypeId = std::int32_t;
using ExprId = std::int32_t;

enum class TypeKind : std::uint8_t {
    Error, // a type the checker could not work out; never in a checked program
    Void,
    Never,
    Bool,
    U8,
    I8,
    U16,
    I16,
    Ptr,
    Array,
    Struct,
    Enum,
};

struct Type {
    TypeKind kind = TypeKind::Error;
    TypeId elem = -1;        // ptr, array
    std::uint32_t count = 0; // array
    std::int32_t decl = -1;  // struct or enum index
    std::uint32_t size = 0;  // bytes
};

// fixed ids, interned first
constexpr TypeId kErrorType = 0;
constexpr TypeId kVoidType = 1;
constexpr TypeId kNeverType = 2;
constexpr TypeId kBoolType = 3;
constexpr TypeId kU8Type = 4;
constexpr TypeId kI8Type = 5;
constexpr TypeId kU16Type = 6;
constexpr TypeId kI16Type = 7;

struct Field {
    std::string name;
    TypeId type = -1;
    std::uint32_t offset = 0; // packed byte offset
};

struct StructDecl {
    std::string name; // as declared, Sprite
    std::int32_t module = -1;
    Loc loc;
    std::vector<Field> fields;
    std::uint32_t size = 0;
};

struct EnumMember {
    std::string name;
    std::uint8_t value = 0;
};

struct EnumDecl {
    std::string name;
    std::int32_t module = -1;
    Loc loc;
    std::vector<EnumMember> members;
};

enum class VarKind : std::uint8_t {
    Global,     // top-level let
    ConstArray, // (const NAME (array t n) ...), read only
    Param,
    Local,
};

// :address, :align, :boundary, :in aux and :reserve on a top-level let; 0 means not given
struct Placement {
    bool has_address = false;
    std::uint16_t address = 0;
    std::uint32_t align = 0;
    std::uint32_t boundary = 0;
    bool in_aux = false;  // :in aux: in auxiliary memory, reached only through lib/aux
    bool reserve = false; // :reserve: memory the program reaches by address; not a variable
};

struct Var {
    std::string name; // qualified for globals (gfx/hgr.row_lo), plain for locals
    VarKind kind = VarKind::Global;
    TypeId type = -1;
    std::int32_t module = -1;
    std::int32_t function = -1; // owner of params and locals
    Loc loc;
    Placement placement;
    bool address_taken = false; // some addr_of reaches into it: stored packed
    bool loop_var = false;
    std::vector<std::uint8_t> init; // packed bytes; empty means zero
};

enum class Op : std::uint8_t {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    And,
    Or,
    Xor,
    Shl,
    Shr,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    BitNot,
    Not,
};

enum class ExprKind : std::uint8_t {
    Literal,     // value (raw bits): integers, bool 0/1, enum members, folded consts
    VarRef,      // ref: var
    Index,       // a: array place, b: index
    Field,       // a: struct place or Load, ref: field index, value: packed offset
    Load,        // a: pointer
    Store,       // a: pointer, b: value
    LoadField,   // (load p pos x): a: pointer, value: byte offset of the field, type: the field's
    StoreField,  // (store p pos x v): a: pointer, b: value, value: byte offset, ref: field TypeId
    AddrOf,      // a: place
    Unary,       // op, a
    Binary,      // op, a, b; comparisons have type bool, operand type is exprs[a].type
    And,         // a, b: short circuit
    Or,          // a, b: short circuit
    PtrAdd,      // a: pointer, b: u16 count, value: element size
    PtrSub,      // a: pointer, b: u16 count, value: element size
    Convert,     // a: integer, to type
    Reinterpret, // as: a between u16 and a pointer, same bits
    Call,        // ref: function, list: arguments
    IoRead,      // a: u16 address; type u8 or u16
    IoWrite,     // a: address, b: value, value: bytes (1 or 2)
    Jsr,         // a: u16 address, b: A's value or -1, c: JsrRegs or -1; type u8 (A after)
    JsrRegs,     // only as a Jsr's c: a: X's value or -1, b: Y's value or -1; type void
    StructLit,   // list: one value per field, in order
    If,          // a: cond, b: then, c: else or -1
    Do,          // list: forms; value is the last
    While,       // a: cond, b: body (a Do)
    For,         // ref: loop var, a: start, b: end or -1 for "through the type's max", c: body
    Match,       // a: scrutinee, clauses: first and count
    Return,      // a: value or -1
    Break,
    Continue,
    Let, // ref: var, a: initial value (locals always have one)
    Set, // a: place, b: value
};

struct Expr {
    ExprKind kind = ExprKind::Literal;
    Op op = Op::Add;
    TypeId type = kErrorType;
    ExprId a = -1;
    ExprId b = -1;
    ExprId c = -1;
    std::int32_t ref = -1;
    std::uint32_t first = 0; // into Program::lists or Program::clauses
    std::uint32_t count = 0;
    std::uint32_t value = 0;
    Loc loc;
};

struct Clause {
    bool wildcard = false;
    std::uint16_t value = 0; // raw bits of the pattern
    ExprId body = -1;
    Loc loc;
};

struct Function {
    std::string name; // qualified: gfx/hgr.plot
    std::int32_t module = -1;
    Loc loc;
    std::vector<std::int32_t> params; // vars
    std::vector<std::int32_t> locals; // vars, loop variables included
    TypeId ret = kVoidType;
    ExprId body = -1;
    std::vector<std::int32_t> callees; // functions called directly, no repeats
};

struct ModuleInfo {
    std::string name; // main, gfx/hgr, apple2
    std::uint32_t file = 0;
};

struct Program {
    Sources sources;
    std::vector<ModuleInfo> modules;
    std::int32_t entry_module = 0;
    std::vector<Type> types;
    std::vector<StructDecl> structs;
    std::vector<EnumDecl> enums;
    std::vector<Var> vars;
    std::vector<Function> functions;
    std::vector<Expr> exprs;
    std::vector<ExprId> lists;
    std::vector<Clause> clauses;
    std::int32_t main = -1; // main in the entry module, if any
};

bool IsIntegerKind(TypeKind kind);
bool IsSignedKind(TypeKind kind);
// fits in a register pair: integers, bool, enums, pointers
bool IsScalarKind(TypeKind kind);
std::uint32_t KindBits(TypeKind kind); // 8 or 16 for scalars

const Type& TypeOf(const Program& program, TypeId id);
TypeKind KindOf(const Program& program, TypeId id);
std::string TypeName(const Program& program, TypeId id);

std::int32_t FindFunction(const Program& program, const std::string& qualified);
std::int32_t FindVar(const Program& program, const std::string& qualified);
std::int32_t FindFieldIndex(const Program& program, TypeId struct_type, const std::string& name);

// the raw bits of a constant expression, folded with the target's arithmetic; false when it
// is not constant (a division by zero counts as not constant)
bool ConstValue(const Program& program, ExprId id, std::uint16_t& value);

} // namespace tisple
