#pragma once

// Code generation straight from the typed tree to 6502 items (no separate IR).
//
// - A value is read through an Opnd: per byte, an immediate, a memory byte, an indexed
//   memory byte (abs,X / abs,Y), a (zp),Y byte, or the register a loop variable lives in.
//   Simple expressions (variables, constants, fields, elements with a u8 index, loads through
//   a zero-page pointer, free conversions) become an Opnd without any code.
// - Computed 8-bit values end in A; computed 16-bit values in a frame temporary or straight
//   in their destination. Results of calls come back in A or A/X.
// - Conditions compile to branches (GenBranch), never to booleans first.
// - X and Y hold index values; a small tracker remembers what they hold, so an index is
//   loaded once for a run of accesses. A u8 `for` counter may live in X or Y ("pinned"):
//   the body is generated once per choice and kept if it never touches that register.
// - Temporaries are frame bytes of the function, so they survive calls (callee frames never
//   overlap the caller's). Pointer temporaries always live in zero page.

#include "asm/asm.hpp"
#include "backend/analysis.hpp"
#include "backend/backend.hpp"
#include "backend/effects.hpp"

#include <string>
#include <vector>

namespace tisple {

enum class SymRole : std::uint8_t {
    Label,
    Var,      // owner: var id
    Temps,    // owner: function; value temporaries
    PtrTemps, // owner: function; pointer temporaries, always zero page
    LoopPtrs, // owner: function; derived pointers of wide loops, always zero page
    Scratch,  // fixed zero page for the runtime and single instruction sequences
    Data,     // tables, the zero-page image, the save buffer
};

struct SymInfo {
    SymRole role = SymRole::Label;
    std::int32_t owner = -1;
    std::uint64_t weight = 0; // uses, times 4 per loop level
};

enum class Runtime : std::uint8_t {
    Mul8,   // A * rt_b -> A
    Mul16,  // rt_a * rt_b -> A/X
    DivU8,  // rt_a / rt_b -> A, remainder in rt_r
    DivU16, // rt_a / rt_b -> rt_a, remainder in rt_r
    DivS8,  // signed, same places
    DivS16,
    Count,
};
constexpr int kRuntimeCount = static_cast<int>(Runtime::Count);

struct Backend {
    const Program* program = nullptr;
    Layout shapes; // every var's shape, address 0
    Assembly as;   // the symbols; items are put together at the end
    std::vector<SymInfo> info;
    std::vector<std::int32_t> var_sym;
    std::vector<std::int32_t> fn_sym;
    std::vector<std::int32_t> temp_sym;
    std::vector<std::int32_t> ptemp_sym;
    std::vector<std::uint32_t> temp_size;
    std::vector<std::uint32_t> ptemp_size;
    std::vector<std::int32_t> lptr_sym;
    std::vector<std::uint32_t> lptr_size;
    Effects effects;
    std::vector<bool> deref;                // by var: some load or store goes straight through it
    std::vector<std::vector<AsmItem>> code; // by function
    std::int32_t scratch = -1;              // 2 bytes
    std::int32_t rt_a = -1;                 // 2 bytes each
    std::int32_t rt_b = -1;
    std::int32_t rt_r = -1;
    std::int32_t rt_s = -1; // 1 byte
    std::int32_t rt_sym[kRuntimeCount] = {};
    bool rt_used[kRuntimeCount] = {};
    std::vector<Diagnostic>* errors = nullptr;
};

std::int32_t NewSym(Backend& be, const std::string& name, SymRole role, std::int32_t owner);
// a pointer var that lives in zero page, so (p),Y reads through it directly
bool PointerInZp(const Backend& be, std::int32_t var);

// ---- operands

enum class ByteKind : std::uint8_t {
    Imm,
    Mem,
    MemIdx, // value + an index register holding Opnd::index
    IndY,   // (value),Y: Y is `y`, or Opnd::index when y_index
    Reg,    // the pinned register `reg` (0 = X, 1 = Y)
};

struct ByteRef {
    ByteKind kind = ByteKind::Imm;
    AsmValue value;
    std::uint8_t y = 0;
    bool y_index = false;
    std::uint8_t reg = 0;
};

// what an index register must hold: a memory byte, or a pinned register
struct Index {
    bool used = false;
    bool is_reg = false;
    std::uint8_t reg = 0;
    AsmValue value; // memory byte
};

struct Opnd {
    std::uint8_t size = 0; // 0: none
    bool in_a = false;     // 8-bit value in A right now
    bool temp = false;     // lives in a temporary nobody else writes
    bool moved = false;    // some byte comes from another byte position: (<< x 8)
    ByteRef b[2];
    Index index;
};

Opnd ImmOpnd(std::uint16_t value, std::uint32_t size);
Opnd SymImmOpnd(AsmValue address, std::uint32_t size); // #<sym, #>sym
Opnd MemOpnd(AsmValue address, std::uint32_t size, std::uint32_t stride);
Opnd InA();
bool IsImm(const Opnd& o);
// later code could change what it reads
bool IsVolatile(const Opnd& o);

// ---- per-function generator state

struct RegContent {
    bool known = false;
    bool mem = false; // the register equals the memory byte at v (else the constant v)
    AsmValue v;
};

struct LoopCtx {
    std::int32_t break_label = -1;
    std::int32_t continue_label = -1;
};

// a zero-page pointer kept at base + (i & $FF00) for a u16 loop counter i whose low byte
// lives in Y, so base + i is (ptr),Y; it moves up a page when the counter's high byte does
struct Derived {
    AsmValue base;           // a symbol's address, a number, or (var) a pointer variable
    std::int32_t var = -1;   // the pointer variable, if the base is one
    std::int32_t offset = 0; // added to the base
    AsmValue ptr;
};

// the u16 loop counter in Y (low byte) and memory (high byte)
struct WideLoop {
    std::int32_t var = -1;
    AsmValue high;
    ExprId body = -1;
    // made during the body, trial runs included, so slots are never handed out twice
    std::vector<Derived> derived;
    std::uint32_t next_slot = 0;
};

struct Gen {
    Backend* be = nullptr;
    const Program* program = nullptr;
    std::int32_t function = -1;
    std::vector<AsmItem>* out = nullptr;
    RegContent reg[2];              // X, Y
    std::int32_t pin[2] = {-1, -1}; // loop var living in X, Y
    WideLoop* wide = nullptr;       // the u16 loop var whose low byte is pin[1]
    bool violation = false;         // code touched a pinned register
    int loop_depth = 0;
    int for_depth = 0;
    std::vector<LoopCtx> loops;
    std::uint32_t temp_top = 0;
    std::uint32_t ptemp_top = 0;
    std::uint32_t last_file = 0xFFFFFFFF;
    std::uint32_t last_line = 0;
    bool failed = false;
};

void Fail(Gen& g, const Loc& loc, const std::string& message);
void Unsupported(Gen& g, ExprId id, const char* what);

// ---- emission (gen_emit.cpp)

void Emit(Gen& g, Mn mn, Mode mode, AsmValue value = AsmValue());
std::int32_t NewLabel(Gen& g);
void PlaceLabel(Gen& g, std::int32_t label);
void Branch(Gen& g, Mn mn, std::int32_t label);
void Jump(Gen& g, std::int32_t label);
void ForgetRegs(Gen& g);
void SourceNote(Gen& g, const Loc& loc);

void LoadA(Gen& g, const Opnd& o, int k);
void OpA(Gen& g, Mn mn, const Opnd& o, int k); // ADC SBC AND ORA EOR CMP with byte k
void StoreA(Gen& g, const Opnd& dst, int k);
// INC DEC ASL LSR ROL ROR on byte k of dst; false if that byte has no such mode
bool RmwByte(Gen& g, Mn mn, const Opnd& dst, int k);
bool CanRmw(const Gen& g, const Opnd& dst, int k);
void LoadIndexReg(Gen& g, int reg, const Index& index);
void LoadYConst(Gen& g, std::uint8_t value);
void LoadXConst(Gen& g, std::uint8_t value);

struct TempMark {
    std::uint32_t temp = 0;
    std::uint32_t ptemp = 0;
};
TempMark MarkTemps(const Gen& g);
void ReleaseTemps(Gen& g, TempMark mark);
Opnd NewTemp(Gen& g, std::uint32_t size);
AsmValue NewPtrTemp(Gen& g); // 2 zero-page bytes

void CopyOpnd(Gen& g, const Opnd& src, const Opnd& dst);
// a 16-bit operand into A (low) and X (high)
void LoadAX(Gen& g, const Opnd& o);
void StoreAX(Gen& g, const Opnd& dst);
// A holds an 8-bit value: keep it in a temporary
Opnd SpillA(Gen& g);
// writing dst byte by byte from low to high could overwrite a byte of src that is read
// later (only byte-moving operands make this possible; pointers are always aligned to what
// they were made from, see effects.hpp)
bool CrossAliases(const Opnd& src, const Opnd& dst);
// may the operands share memory at all
bool Overlaps(const Opnd& a, const Opnd& d);
// may running `code` (-1: nothing) change what o reads
bool MayWriteOpnd(const Gen& g, ExprId code, const Opnd& o);
// may running `code` change var
bool MayWriteVarIn(const Gen& g, ExprId code, std::int32_t var);
// copy to a temporary unless it is already safe from later code
Opnd Snapshot(Gen& g, const Opnd& o);

// ---- places (gen_place.cpp)

// the address of a place or of what a pointer points at, as code generation sees it:
// byte k of a scalar is at base + cursor.address + k * cursor.stride (+ index * mult)
struct PlaceRef {
    bool pointer = false; // base is the zero-page pointer at `ptr`
    AsmValue base;        // static base: the var's symbol
    AsmValue ptr;
    AsmValue ptr2; // pointer for byte 1 when it is 256 or more bytes away; sym -1: none
    std::int32_t root = -1;
    Cursor cursor;
    bool has_index = false;
    Opnd index; // u8 or u16 value
    std::uint32_t mult = 0;
    TypeId type = -1;
    // where ptr came from: a pointer variable, or a constant address
    std::int32_t ptr_var = -1;
    bool ptr_const = false;
    std::uint16_t ptr_value = 0;
};

// evaluates the index and pointer parts of a place, in order, keeping in temporaries every
// value the `later` code (-1: none) could change before the place is used
bool ResolvePlace(Gen& g, ExprId id, ExprId later, PlaceRef& out);
// the place a store or store-field writes
bool ResolveStore(Gen& g, const Expr& store, ExprId later, PlaceRef& out);
// emits pointer arithmetic so every byte of a value of type `type` is directly reachable
void NormalizePlace(Gen& g, PlaceRef& place, TypeId type);
// an operand for the scalar at the place (after NormalizePlace)
Opnd PlaceOpnd(Gen& g, const PlaceRef& place, TypeId type);
// a sub-place: field at packed offset, element with a constant index
PlaceRef FieldPlace(const Gen& g, const PlaceRef& place, std::uint32_t offset);
PlaceRef ElementPlace(const Gen& g, const PlaceRef& place, TypeId array_type, std::uint32_t i);
// an expression that needs no code to read, or size 0
Opnd TrySimple(Gen& g, ExprId id);
// &place as a 16-bit value
Opnd AddressOf(Gen& g, ExprId place, const Opnd* dst);
void CopyStruct(Gen& g, ExprId value, const PlaceRef& dst, TypeId type);

// ---- expressions (gen_expr.cpp, gen_arith.cpp)

void EvalA(Gen& g, ExprId id);
// an operand holding the value; computed values go to dst when given (and safe), else to a
// temporary. 8-bit computed values may come back in_a.
Opnd Eval(Gen& g, ExprId id, const Opnd* dst);
// the value, written into dst
void EvalInto(Gen& g, ExprId id, const Opnd& dst);
Opnd EvalCall(Gen& g, ExprId id, const Opnd* dst);
void EvalArith8(Gen& g, const Expr& e);
Opnd EvalArith16(Gen& g, const Expr& e, const Opnd* dst);
// 16-bit a op b into dst, byte by byte (ADC/SBC with carry, AND/ORA/EOR)
void Combine16(Gen& g, Op op, const Opnd& a, const Opnd& b, const Opnd& dst);
// multiply a 16-bit operand by a constant into dst
void MulConst16(Gen& g, const Opnd& a, std::uint32_t factor, const Opnd& dst);
void CallRuntime(Gen& g, Runtime routine);
// io_read or io_write as a statement
void GenIo(Gen& g, ExprId id);

// ---- conditions (gen_cond.cpp)

// jumps to label when the bool expression equals `when`, else falls through
void GenBranch(Gen& g, ExprId id, bool when, std::int32_t label);
// jumps to label when a op b; both operands can be read without side effects
void CompareJump(Gen& g, Op op, bool is_signed, Opnd a, Opnd b, std::int32_t label);
// a condition as 0 or 1 in A
void CondToA(Gen& g, ExprId id);

// ---- statements (gen_stmt.cpp, gen_loop.cpp)

void GenStmt(Gen& g, ExprId id);
void GenFor(Gen& g, const Expr& e);
// the derived pointer for base + offset in the current wide loop (made on first use)
AsmValue DerivedPointer(Gen& g, AsmValue base, std::int32_t var, std::int32_t offset);
void GenWhile(Gen& g, const Expr& e);
void GenMatch(Gen& g, const Expr& e, const Opnd* dst, bool value);
void GenSet(Gen& g, ExprId place, ExprId value);
void GenStore(Gen& g, const Expr& e);
void GenFunction(Backend& be, std::int32_t function);

} // namespace tisple
