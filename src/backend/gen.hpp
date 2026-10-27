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
// - X and Y hold index values; a small tracker remembers what they hold (a memory byte plus
//   a small delta), so an index is loaded once for a run of accesses and x + 1 after x is
//   INY. A byte index (a u8, maybe widened, plus or minus constants) on a pointer is (p),Y
//   with no address arithmetic (gen_index.cpp).
// - A u8 `for` counter may live in X or Y ("pinned"), or in memory when the body has loops
//   of its own: the body is generated once per choice, a choice that touches its register is
//   dropped, and the cheapest kept, cycles inside loops weighing more. With the counter in Y,
//   p + counter + k is (p + k),Y through a loop pointer set before the loop.
// - A while loop may keep byte variables in X and Y, or a u16 one in Y and memory, the same
//   way (gen_while.cpp); the loop's sets of them write the register, and it is stored back
//   after the loop.

// - Temporaries are frame bytes of the function, so they survive calls (callee frames never
//   overlap the caller's). Pointer temporaries always live in zero page.

#include "asm/asm.hpp"
#include "backend/analysis.hpp"
#include "backend/backend.hpp"
#include "backend/effects.hpp"
#include "backend/index_alias.hpp"
#include "backend/ranges.hpp"

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
    Mul8x8, // A * rt_b -> A/X: bytes, the whole product
    Count,
};
constexpr int kRuntimeCount = static_cast<int>(Runtime::Count);

// a division routine for one constant divisor (gen_div.cpp)
struct DivRoutine {
    std::uint32_t divisor = 0;
    bool wide = false;
    std::int32_t sym = -1;
};

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
    std::vector<std::uint32_t> fn_exprs;  // by function: expressions in its body
    std::vector<bool> deref;              // by var: some load or store goes straight through it
    std::vector<IndexAlias> aliases;      // by var: locals that only name a byte index
    std::vector<bool> div_pairs;          // by expr: FindDivPairs
    std::vector<DivRoutine> div_routines; // the ones the code calls
    ValueRanges ranges;
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

// what an index register must hold: a memory byte, or a pinned register, plus delta
struct Index {
    bool used = false;
    bool is_reg = false;
    std::uint8_t reg = 0;
    AsmValue value;         // memory byte
    std::uint8_t delta = 0; // added mod 256 (INX/DEX, INY/DEY)
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
    std::uint8_t delta = 0; // mem: plus this, mod 256
};

struct LoopCtx {
    std::int32_t break_label = -1;
    std::int32_t continue_label = -1;
};

// a call being inlined: its returns jump to ret_label with the value in A or in dst
struct InlineCtx {
    std::int32_t ret_label = -1;
    Opnd dst; // 16-bit results
};

// a zero-page pointer for a loop whose counter lives in Y, so base + offset + counter is
// (ptr),Y with no address arithmetic in the loop; it is set just before the loop
// - u16 counter i (low byte in Y): ptr = base + offset + (i & $FF00); it moves up a page when
//   the counter's high byte does
// - u8 counter: ptr = base + offset, used only where counter + offset never wraps (a constant
//   delta on a byte index, see CounterRange)
// - step pointer (owner set): ptr = base + offset + v for a u16 variable v in memory that the
//   owner loop only steps by constants; it moves with every step (gen_step.cpp)
struct Derived {
    std::int32_t loop = -1;  // the loop's counter variable
    AsmValue base;           // a symbol's address, a number, or (var) a pointer variable
    std::int32_t var = -1;   // the pointer variable, if the base is one
    std::int32_t offset = 0; // added to the base
    AsmValue ptr;
    ExprId owner = -1; // a step pointer's loop
};

// the pointer of one (at table i) with a u16 i: its low byte is the table's, stored once where
// the function starts, its high byte the table's plus i's, and Y holds i's low byte
// (gen_index.cpp, UsePagePointer)
struct PagePointer {
    ExprId site = -1;
    AsmValue base; // the table's address, with the place's offset
    AsmValue ptr;
};

// a u16 variable a loop steps only by constants (set v (+ v c)), in memory: its arrays and
// pointers are read through step pointers (gen_step.cpp)
struct SteppedVar {
    std::int32_t var = -1;
    ExprId loop = -1; // the outermost such loop being generated
};

// where a step pointer must move with its variable: just before the label
struct StepFixup {
    std::int32_t label = -1;
    std::int32_t var = -1;
    std::uint16_t step = 0;
};

// the u16 loop counter in Y (low byte) and memory (high byte)
struct WideLoop {
    std::int32_t var = -1;
    AsmValue high;
    ExprId body = -1;
};

// a let of a u8 loop's body that moves by a constant each iteration (gen_reduce.cpp)
struct ReducedLet {
    std::int32_t var = -1;
    ExprId value = -1;
    std::uint16_t step = 0;
};

// a byte expression of a u8 loop's body that moves by a constant each iteration, or not at all
// (gen_reduce_byte.cpp); while its loop is generated, reading it reads the temporary
struct ReducedByte {
    ExprId expr = -1;
    std::uint8_t step = 0;
    Opnd temp;
};

// the values a u8 loop counter takes inside its body (loop variables are read-only)
struct CounterRange {
    std::int32_t var = -1;
    int lo = 0;
    int hi = 255;
};

// what a condition tells about a u8 variable where it holds: inside an if's arm, a while's
// body, the second half of an and; dead once a statement that may change the variable
// starts (gen_facts.cpp)
struct Fact {
    std::int32_t var = -1;
    int lo = 0;
    int hi = 255;
    bool live = true;
};

// where a wide while loop's derived pointers must follow a change of its variable's high
// byte: just before the label, once the loop's pointers are known (gen_while.cpp)
enum class WideFix : std::uint8_t {
    Inc,     // the high byte went up by one
    Dec,     // down by one
    Compute, // anything: each pointer's high byte from the base's and the variable's
};

struct WideFixup {
    std::int32_t label = -1;
    WideFix fix = WideFix::Inc;
};

// the last division's operands: its quotient is in rt_a, and its remainder in rt_r when
// have_rem, until code touches them (gen_div.cpp)
struct DivDone {
    bool valid = false;
    ExprId a = -1;
    ExprId b = -1;
    std::size_t at = 0; // the items from here on
    bool have_rem = false;
    bool rem_byte = false; // the remainder is below 256: rt_r+1 is not set
};

struct Gen {
    Backend* be = nullptr;
    const Program* program = nullptr;
    std::int32_t function = -1;
    std::vector<AsmItem>* out = nullptr;
    RegContent reg[2];                // X, Y
    std::int32_t pin[2] = {-1, -1};   // loop var living in X, Y
    ExprId pin_body[2] = {-1, -1};    // the body of that loop
    WideLoop* wide = nullptr;         // the u16 loop var whose low byte is pin[1]
    std::vector<Derived> derived;     // of the loops being generated
    std::uint32_t lptr_top = 0;       // loop pointer bytes handed out in this function
    std::vector<CounterRange> ranges; // of the u8 loops being generated
    std::vector<Fact> facts;
    std::vector<WideFixup> wide_fixups; // of the wide while loop being generated
    std::vector<SteppedVar> stepped;
    std::vector<StepFixup> step_fixups;
    std::vector<PagePointer> page_ptrs;
    int while_pins = 0; // while loops being generated with variables pinned
    DivDone div;
    std::vector<std::int32_t> reduced; // vars of lets worked out before their loop
    std::vector<ReducedByte> reduced_bytes;
    // parameters of the calls being inlined that read their argument, a plain variable the
    // callee never changes, instead of a copy of it
    std::vector<std::pair<std::int32_t, ExprId>> bound;
    std::uint64_t cost = 0; // cycles emitted so far, times 4 per loop level
    bool violation = false; // code touched a pinned register
    int loop_depth = 0;
    int for_depth = 0;
    std::vector<LoopCtx> loops;
    std::vector<InlineCtx> inlines;
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
// the last item leaves the carry clear for certain: a CLC, or an add marked as not carrying
bool CarryClearNow(const Gen& g);
// A holds byte k of o: the last instruction stored it there or loaded it from there
bool AHoldsByte(const Gen& g, const Opnd& o, int k);
// the largest value id can have (ranges.hpp)
std::uint32_t MaxOf(const Gen& g, ExprId id);
// byte k of o as a number known now: a plain number, or a byte of a symbol already placed
// (an :address variable); -1 if it is not one
int KnownImm(const Gen& g, const Opnd& o, int k);
std::int32_t NewLabel(Gen& g);
void PlaceLabel(Gen& g, std::int32_t label);
// a label only forward branches over straight-line code reach, which leave X and Y alone:
// what they hold stays known
void PlaceLocalLabel(Gen& g, std::int32_t label);
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
bool CrossAliases(const Gen& g, const Opnd& src, const Opnd& dst);
// may the operands share memory at all
bool Overlaps(const Gen& g, const Opnd& a, const Opnd& d);
// may writing byte y of dst change what reading byte x of src gives: the byte itself, or the
// index or pointer bytes its address comes from
bool MayChangeRead(const Gen& g, const Opnd& src, int x, const Opnd& dst, int y);
// a store through a pointer can reach this byte (pointer provenance, effects.hpp)
bool PointerMayReach(const Gen& g, const AsmValue& v);
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
    // a one-byte index stands for (index + delta) & $FF; counter: the u8 loop variable it
    // reads, if any (its CounterRange may let the delta move into the address)
    std::uint8_t delta = 0;
    std::int32_t counter = -1;
    // ptr is a step pointer, which later steps move: not a temporary of this place's own
    bool step_ptr = false;
    ExprId site = -1; // the Index expression the index came from
};

// evaluates the index and pointer parts of a place, in order, keeping in temporaries every
// value the `later` code (-1: none) could change before the place is used
bool ResolvePlace(Gen& g, ExprId id, ExprId later, PlaceRef& out);
// the place a store or store-field writes
bool ResolveStore(Gen& g, const Expr& store, ExprId later, PlaceRef& out);
// emits pointer arithmetic so every byte of a value of type `type` is directly reachable
void NormalizePlace(Gen& g, PlaceRef& place, TypeId type);
// a store of a value of this type to the place writes its bytes one by one: keep the index
// and pointer it goes through in temporaries when one of those bytes may be them
void PinPlace(Gen& g, PlaceRef& place, TypeId type);
// an operand for the scalar at the place (after NormalizePlace)
Opnd PlaceOpnd(Gen& g, const PlaceRef& place, TypeId type);
// a sub-place: field at packed offset, element with a constant index
PlaceRef FieldPlace(const Gen& g, const PlaceRef& place, std::uint32_t offset);
PlaceRef ElementPlace(const Gen& g, const PlaceRef& place, TypeId array_type, std::uint32_t i);
// the place's parts without emitting anything; false if that needs code
bool ResolveForRead(Gen& g, ExprId id, PlaceRef& out);
bool IsNormalPlace(const Gen& g, const PlaceRef& place, TypeId type);
// the place's address so far into a zero-page pointer temporary, or into `into` (plain
// memory) when that is safe; place.ptr says where it went
void PlaceToPointer(Gen& g, PlaceRef& place, const Opnd* into = nullptr);
// an expression that needs no code to read, or size 0 (gen_simple.cpp)
Opnd TrySimple(Gen& g, ExprId id);

// ---- byte indices and loop pointers (gen_index.cpp)

// a u8 value, or a u16 one below 256, that reads as (base + delta) & $FF with no code
struct ByteIndex {
    Opnd base; // one byte: memory, a pinned register or a number
    std::uint8_t delta = 0;
    std::int32_t counter = -1; // the loop variable base is, if any
};

// id, or the value of the index alias it names
ExprId AliasTarget(const Gen& g, ExprId id);
// id, or the value of the forwarded let it reads
ExprId ForwardTarget(const Gen& g, ExprId id);
// id, or the argument a bound parameter reads (Gen::bound)
ExprId BoundTarget(const Gen& g, ExprId id);
bool ByteIndexOf(Gen& g, ExprId id, ByteIndex& out);
// INX/INY or DEX/DEY reach it
bool IsSmallDelta(std::uint8_t delta);
// counter + delta never wraps while the counter's loop body runs
bool DeltaFits(const Gen& g, std::int32_t counter, std::uint8_t delta);
// the place as (derived),Y in the loop whose counter is in Y; no code now, the pointer is
// set before the loop. False if the place is not one.
bool UseLoopPointer(Gen& g, PlaceRef& place, TypeId type);
// a byte of a table at a u16 index in memory as (page pointer),Y: the pointer's high byte
// worked out now, Y the index's low byte. False if the place is not one.
bool UsePagePointer(Gen& g, PlaceRef& place, TypeId type);
// each page pointer's low byte, stored where the function starts
void InitPagePointers(Gen& g);
// the derived pointer for base + offset in the loop of counter `loop` (made on first use)
AsmValue DerivedPointer(Gen& g, std::int32_t loop, AsmValue base, std::int32_t var,
                        std::int32_t offset);
// the loop's derived pointers that its code (items from `first` on) reads; the loop's others
// are dropped
std::vector<Derived> TakeDerived(Gen& g, std::int32_t loop, std::size_t first);
// code setting each pointer, moved to item `at`, just before the loop; high is the u16
// counter's high byte, or sym -1 for a u8 counter
void InitDerived(Gen& g, const std::vector<Derived>& derived, std::size_t at, AsmValue high);
// the pointers following a change of the counter's high byte (only A and the carry)
void FollowHigh(Gen& g, const std::vector<Derived>& derived, WideFix fix, AsmValue high);

// ---- step pointers (gen_step.cpp)

// the u16 variables `loop` steps only by constants and no loop around it does; their step
// pointers belong to this loop. counter: also a for loop's own u16 counter in memory (-1:
// none). Returns the mark EndStepped takes.
std::size_t BeginStepped(Gen& g, ExprId loop, std::int32_t counter);
// after the loop: its step pointers set at item `at` (before the loop), and moved at every
// step of their variable
void EndStepped(Gen& g, ExprId loop, std::size_t mark, std::size_t at);
// the address base + v (v a stepped variable) as its step pointer; false if it is not one
bool UseStepPointer(Gen& g, PlaceRef& place);
// the index is a stepped variable's two bytes
bool IsSteppedIndex(const Gen& g, const Opnd& index);
// var was just stepped by `step` (a set, or a for loop's own count): its pointers move here
void NoteStep(Gen& g, std::int32_t var, std::uint16_t step);
// (set place value) just ran: a step of a stepped variable moves its pointers
void NoteSet(Gen& g, ExprId place, ExprId value);
// a for loop's own step of its u16 counter in memory, after the body: its pointers move
void StepOwnPointers(Gen& g, ExprId loop, std::int32_t var, std::uint16_t step);

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
// a + b, 16-bit, with bytes known before running needing no code (gen_wide.cpp); the
// result is in dst (or a temporary) except for bytes that are numbers or one operand's byte
Opnd Add16(Gen& g, const Opnd& a, const Opnd& b, const Opnd* dst);
// multiply a 16-bit operand by a constant into dst
void MulConst16(Gen& g, const Opnd& a, std::uint32_t factor, const Opnd& dst);
void CallRuntime(Gen& g, Runtime routine);
// division and remainder (gen_div.cpp): 8-bit into A, 16-bit into dst or a temporary
void EvalDiv8(Gen& g, ExprId id, const Expr& e);
Opnd EvalDiv16(Gen& g, ExprId id, const Expr& e, const Opnd* dst);
// after each statement: forget the last division if the statement may change its operands
void ForgetDivision(Gen& g, ExprId stmt);
// by expr: a division with another of the same operands somewhere in the program
std::vector<bool> FindDivPairs(const Program& program);
// the division routines the code calls, each a label and its items
void EmitDivRoutines(Backend& be, std::vector<AsmItem>& out);
// shifts (gen_shift.cpp): 8-bit into A, 16-bit into dst or a temporary
void EvalShift8(Gen& g, const Expr& e);
Opnd EvalShift16(Gen& g, const Expr& e, const Opnd* dst);
Opnd ShiftConst16(Gen& g, Op op, bool is_signed, const Opnd& a, std::uint16_t count,
                  const Opnd* dst);
// io_read or io_write as a statement
void GenIo(Gen& g, ExprId id);
// (jsr ...): the registers set, the call, A after it (gen_jsr.cpp)
Opnd EvalJsr(Gen& g, ExprId id);

// ---- conditions (gen_cond.cpp)

// jumps to label when the bool expression equals `when`, else falls through
void GenBranch(Gen& g, ExprId id, bool when, std::int32_t label);
// jumps to label when a op b; both operands can be read without side effects
void CompareJump(Gen& g, Op op, bool is_signed, Opnd a, Opnd b, std::int32_t label);
// a condition as 0 or 1 in A
void CondToA(Gen& g, ExprId id);

// ---- statements (gen_stmt.cpp, gen_loop.cpp)

void GenStmt(Gen& g, ExprId id);
void GenFor(Gen& g, ExprId id);
void GenWhile(Gen& g, ExprId id);
// (set var value) when a while loop keeps var in a register; false if it does not
bool SetPinnedVar(Gen& g, ExprId place, ExprId value);
// an instruction that writes the register a loop keeps its variable in, on purpose
void EmitPinned(Gen& g, Mn mn, Mode mode = Mode::Imp, AsmValue value = AsmValue());

// ---- facts (gen_facts.cpp)

// the facts `cond` being `when` gives; with a scope (not -1), only about variables it never
// changes. Returns the mark PopFacts takes.
std::size_t PushFacts(Gen& g, ExprId cond, bool when, ExprId scope);
void PopFacts(Gen& g, std::size_t mark);
// before a statement runs: what it may change is no longer known
void KillFacts(Gen& g, ExprId stmt);
// the values a u8 variable can have here: loop ranges and live facts; false if nothing known
bool VarRange(const Gen& g, std::int32_t var, int& lo, int& hi);
void GenMatch(Gen& g, const Expr& e, const Opnd* dst, bool value);
void GenSet(Gen& g, ExprId place, ExprId value);
void GenStore(Gen& g, const Expr& e);
void GenFunction(Backend& be, std::int32_t function);

// ---- strength reduction (gen_reduce.cpp)

// the lets at the top of a u8 loop's body that are the counter times a constant plus values
// the body never changes; the counter's range must be in Gen::ranges
std::vector<ReducedLet> FindReducedLets(Gen& g, std::int32_t counter, ExprId body);
// each let's value for the first iteration, before the loop's top (the counter holds its
// start); their lets then make no code
void InitReducedLets(Gen& g, const std::vector<ReducedLet>& lets);
// each let moved by its step, at the end of an iteration
void StepReducedLets(Gen& g, const std::vector<ReducedLet>& lets);
// after the loop: their lets make code again
void DropReducedLets(Gen& g, const std::vector<ReducedLet>& lets);
// the same value on every iteration of the loop of counter over body
bool LoopInvariant(Gen& g, std::int32_t counter, ExprId body, ExprId id);

// byte expressions of a u8 or i8 loop's body worth keeping in a temporary that moves by a
// constant (or stays), outside the reduced lets' values (gen_reduce_byte.cpp)
std::vector<ReducedByte> FindReducedBytes(Gen& g, std::int32_t counter, ExprId body,
                                          const std::vector<ReducedLet>& lets);
// each expression's value for the first iteration into its temporary, before the loop's top
// (the counter holds its start); from then on reading it reads the temporary
void InitReducedBytes(Gen& g, std::vector<ReducedByte>& bytes);
// each temporary moved by its step, at the end of an iteration (only A and the flags)
void StepReducedBytes(Gen& g, const std::vector<ReducedByte>& bytes);
void DropReducedBytes(Gen& g, const std::vector<ReducedByte>& bytes);

} // namespace tisple
